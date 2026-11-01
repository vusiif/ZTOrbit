#!/usr/bin/env python3
"""Stop-silence proof: both directions, capture coverage, explicit clock bounds.

Server - local = offset +/- error. Capture must span stop completion through
stop+silence for every allowed offset. The latest possible last packet time
must be <=stop. A packet whose uncertainty interval straddles stop is unproven,
not silently accepted by adding a clock-error allowance to the deadline.
Missing capture/traffic evidence is inconclusive and cannot pass.

The client side is evidence too, not a default:
  * ``statsAfterStop.publisher`` must be present and exactly ``false``;
  * ``statsAfterStop.outboundBytes`` / ``outboundQueued`` must be present and 0;
  * ``stats.localUdpPort`` must be present, non-zero and bound to every uplink
    media flow/event endpoint;
  * a publisher sample speaks for the stopped stream only when its own identity
    is strictly valid: ``publishEpoch`` and ``localUdpPort`` must both be
    present, well formed (epoch > 0, 0 < port <= 65535) and identical between
    the sample top level and the nested ``pipeline`` snapshot. Missing,
    malformed or conflicting identity is *unconfirmed*; it is never silently
    reclassified as another generation;
  * a sample without a numeric ``epochMs`` cannot be placed on either side of
    stop completion: it is *unconfirmed*, never skipped;
  * a sample with ``epochMs > stopCompletedEpochMs`` that belongs to the
    stopped stream must show no publisher activity, no new outbound bytes and
    no queued media. Same-stream activity there is *rejected*. A strictly valid
    other generation is only recorded; it neither proves nor accuses the
    stopped stream;
  * ``epochMs == stopCompletedEpochMs`` (stop+0ms) is the completion boundary
    millisecond: publish activity there is *unconfirmed* because a single
    millisecond cannot order the stop event and the send, while inactivity is
    neutral;
  * "the publisher was active before the requested stop" is decided from the
    sample with the largest ``epochMs`` at or before the request among samples
    whose identity is strictly valid and equal to the stopped stream, never
    from the array order; contradictory publisher states at that same latest
    timestamp are *unconfirmed*. The strictly-valid identity rule applies to
    these pre-stop samples too: missing, malformed or conflicting identity is
    *unconfirmed* and can never prove activity. A strictly valid other
    generation/port is only recorded: it never proves the stopped stream was
    active, and when it is newer than the newest same-stream sample and still
    publishing the last pre-stop state is *unconfirmed* instead of borrowing
    the other stream's activity; it is not rejected on that ground.
Missing role/port evidence is reported as *unconfirmed*; observed same-stream
activity after completion is *rejected*. Neither can silently pass, and a
different generation's samples are never used as proof for the stopped stream.
"""
import argparse
import json
import math
import ipaddress
import re

_MISSING = object()


def evaluate(capture, publisher, offset_ms, offset_error_ms=150., silence_ms=5000.,
             expected_peer=None, expected_port=None):
    stats = publisher.get("stats") or {}
    stop = publisher.get("stopCompletedEpochMs", stats.get("stopCompletedEpochMs"))
    requested = publisher.get("stopRequestedEpochMs", stats.get("stopRequestedEpochMs"))
    start, end = capture.get("startedAtEpochMs"), capture.get("endedAtEpochMs")
    def number(value):
        return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)
    def integer(value):
        if isinstance(value, bool): return None
        try:
            parsed = int(value)
            return parsed if str(parsed) == str(value) or value == parsed else None
        except (ValueError, TypeError, OverflowError): return None
    valid = (all(number(v) for v in (stop, requested, start, end, offset_ms, offset_error_ms, silence_ms))
             and requested <= stop and start < end and offset_error_ms >= 0 and silence_ms >= 5000)
    coverage = bool(valid and start - offset_ms + offset_error_ms <= stop
                    and end - offset_ms - offset_error_ms >= stop + silence_ms)
    missing, rejected = [], []

    def unconfirmed(reason):
        if reason not in missing: missing.append(reason)

    def reject(reason):
        if reason not in rejected: rejected.append(reason)

    last_up, last_down = capture.get("lastUplinkEpochMs"), capture.get("lastDownlinkEpochMs")
    up = last_up - offset_ms if number(last_up) and number(offset_ms) else None
    down = last_down - offset_ms if number(last_down) and number(offset_ms) else None
    # Require that the capture observed both directions (an empty/unbound capture
    # cannot prove a working stream stopped). Inspect buckets as a cross-check.
    seen = up is not None and down is not None
    if not seen: unconfirmed("capture.lastUplinkDownlinkEpochMs")
    late = []
    if valid:
        for b in capture.get("buckets", []):
            t = start + b["startOffsetSeconds"] * 1000 - offset_ms
            if t > stop + offset_error_ms:
                late.append({"startLocalEpochMs": t, "upDatagrams": b.get("upDatagrams", 0),
                             "downDatagrams": b.get("downDatagrams", 0)})
    no_up = bool(seen and valid and up + offset_error_ms <= stop
                 and not any(b["upDatagrams"] for b in late))
    no_down = bool(seen and valid and down + offset_error_ms <= stop
                   and not any(b["downDatagrams"] for b in late))
    if seen and valid:
        if up + offset_error_ms > stop: reject("uplink-observed-after-stop")
        if down + offset_error_ms > stop: reject("downlink-observed-after-stop")
        if any(b["upDatagrams"] for b in late): reject("late-uplink-bucket")
        if any(b["downDatagrams"] for b in late): reject("late-downlink-bucket")
    def relation(value):
        if value is None or stop is None: return "missing"
        if value + offset_error_ms <= stop: return "before-stop"
        if value - offset_error_ms > stop: return "observed-after-stop"
        return "clock-interval-straddles-stop"
    packet_stats = capture.get("kernelPacketStatistics", {}).get("media", {})
    kernel_packets = integer(packet_stats.get("kernelPackets"))
    kernel_drops = integer(packet_stats.get("kernelDrops"))
    complete = kernel_drops == 0 and (kernel_packets or 0) > 0
    if kernel_packets is None or kernel_drops is None:
        unconfirmed("capture.kernelPacketStatistics")
    elif not complete:
        reject("kernel-media-not-complete")
    expected_peer = expected_peer or stats.get("localAddress")
    expected_port = expected_port or stats.get("serverUdpPort")
    expected_epoch = integer(stats.get("publishEpoch"))
    try:
        peer_valid = ipaddress.ip_address(expected_peer).version == 4
    except (ValueError, TypeError):
        peer_valid = False
    port = integer(expected_port)
    local_port = integer(stats.get("localUdpPort"))
    local_port_valid = local_port is not None and 0 < local_port <= 65535
    if "localUdpPort" not in stats:
        unconfirmed("stats.localUdpPort")
    elif not local_port_valid:
        unconfirmed("stats.localUdpPort-invalid")
    if "publishEpoch" not in stats:
        unconfirmed("stats.publishEpoch")
    binding_ok = bool(peer_valid and port is not None and 0 < port <= 65535 and expected_epoch is not None and expected_epoch > 0)
    bound = bool(binding_ok and capture.get("ztPeer") == expected_peer
                 and integer(capture.get("port")) == port
                 and local_port_valid)
    if not binding_ok:
        unconfirmed("publisher.binding-identity")
    elif not local_port_valid:
        # Already reported as a missing local port above; without it the
        # uplink endpoint cannot be tied to this publisher at all.
        unconfirmed("capture.binding-local-port")
    elif capture.get("ztPeer") != expected_peer or integer(capture.get("port")) != port:
        reject("capture-binding-mismatch")
    kernel_time = capture.get("timestampBasis") == "SO_TIMESTAMPNS kernel packet timestamps; read epochs separate"
    if not kernel_time: unconfirmed("capture.timestampBasis")
    registered = all((integer(capture.get(direction, {}).get(counter)) or 0) > 0
                     for direction, counter in (("uplink", "registerDatagrams"), ("downlink", "registerAckDatagrams")))
    if not registered: reject("registration-not-observed")

    # ---- client-side post-stop evidence -------------------------------------
    after = publisher.get("statsAfterStop")
    role_declared = bytes_zero = queued_zero = False
    if not isinstance(after, dict):
        unconfirmed("statsAfterStop")
    else:
        role = after.get("publisher", _MISSING)
        if role is _MISSING:
            unconfirmed("statsAfterStop.publisher")
        elif role is False:
            role_declared = True
        else:
            reject("statsAfterStop.publisher-active")
        for field in ("outboundBytes", "outboundQueued"):
            value = after.get(field, _MISSING)
            if value is _MISSING:
                unconfirmed("statsAfterStop." + field)
                continue
            parsed = integer(value)
            if parsed is None:
                unconfirmed("statsAfterStop." + field + "-invalid")
            elif parsed != 0:
                reject("statsAfterStop.%s=%d" % (field, parsed))
            elif field == "outboundBytes":
                bytes_zero = True
            else:
                queued_zero = True
    client_stopped = bool(isinstance(stats, dict) and stats and role_declared and bytes_zero and queued_zero)

    # Completion-or-later publisher samples. Identity is evidence, not a
    # default: only a strictly valid identity may speak for the stopped flow.
    # Missing, malformed or contradictory identity can never be reclassified
    # as another generation, and a sample without a numeric timestamp cannot
    # be placed relative to stop completion, so both are *unconfirmed*.
    def sample_identity(sample, pipeline):
        """Resolve publishEpoch/localUdpPort or explain why they are unusable."""
        def resolve(name):
            top = sample.get(name, _MISSING)
            nested = pipeline.get(name, _MISSING)
            if top is _MISSING and nested is _MISSING:
                return None, "missing"
            if top is not _MISSING and nested is not _MISSING:
                top_value, nested_value = integer(top), integer(nested)
                if top_value is None or nested_value is None:
                    return None, "invalid"
                if top_value != nested_value:
                    return None, "conflict"
                return top_value, "ok"
            parsed = integer(top if top is not _MISSING else nested)
            return (parsed, "ok") if parsed is not None else (None, "invalid")
        epoch_value, epoch_state = resolve("publishEpoch")
        port_value, port_state = resolve("localUdpPort")
        if epoch_state == "ok" and epoch_value <= 0:
            epoch_state = "invalid"
        if port_state == "ok" and not 0 < port_value <= 65535:
            port_state = "invalid"
        problem = None
        if epoch_state != "ok":
            problem = "publisherSamples.publishEpoch-" + epoch_state
        elif port_state != "ok":
            problem = "publisherSamples.localUdpPort-" + port_state
        return epoch_value, port_value, problem

    post_stop_samples = []
    for sample in publisher.get("publisherSamples", []) or []:
        if not isinstance(sample, dict):
            unconfirmed("publisherSamples.entry")
            continue
        stamp = sample.get("epochMs")
        pipeline = sample.get("pipeline") if isinstance(sample.get("pipeline"), dict) else {}

        def value_of(name):
            return sample.get(name, pipeline.get(name, _MISSING))
        outbound = integer(value_of("outboundBytes"))
        queued = integer(value_of("outboundQueued"))
        publishing = bool(value_of("publisher") is True or pipeline.get("publisher") is True)
        activity = bool(publishing or (outbound or 0) > 0 or (queued or 0) > 0)
        epoch_seen, port_seen, identity_problem = sample_identity(sample, pipeline)
        if not number(stamp):
            # No clock reading: this sample can neither prove silence nor be
            # skipped, so the proof stays unconfirmed.
            unconfirmed("publisherSamples.timestamp-missing")
            post_stop_samples.append(
                {"epochMs": stamp, "identity": "unconfirmed", "publishEpoch": epoch_seen,
                 "localUdpPort": port_seen, "publisher": publishing,
                 "outboundBytes": outbound, "outboundQueued": queued, "active": activity,
                 "boundary": False, "reason": "timestamp-missing"})
            continue
        if not number(stop) or stamp < stop:
            continue  # pre-stop sample: judged by the largest epochMs below
        boundary = stamp == stop
        identity = "unconfirmed"
        if identity_problem is None and binding_ok:
            identity = ("same" if (epoch_seen == expected_epoch and port_seen == local_port)
                        else "other")
        entry = {"epochMs": stamp, "identity": identity, "publishEpoch": epoch_seen,
                 "localUdpPort": port_seen, "publisher": publishing,
                 "outboundBytes": outbound, "outboundQueued": queued, "active": activity,
                 "boundary": boundary}
        post_stop_samples.append(entry)
        if identity_problem is not None:
            unconfirmed(identity_problem)
        elif activity and identity == "same":
            if boundary:
                # stop+0ms: the stop and the send share one millisecond, so
                # neither a clean stop nor a late send is provable.
                unconfirmed("publisherSamples.activity-at-stop-completion@%s" % stamp)
            else:
                reject("post-stop-activity@%s" % stamp)
    if not publisher.get("publisherSamples"):
        unconfirmed("publisherSamples")

    flows = capture.get("independentMediaFlows", {})
    flow_results = []
    for name, flow in flows.items():
        last = flow.get("lastKernelEpochMs")
        read = flow.get("lastReadEpochMs")
        match = re.fullmatch(r"(up|down):([0-9.]+):([0-9]+):epoch=([0-9]+)", name)
        flow_bound = False
        if match:
            direction, address, endpoint_port, epoch = match.groups()
            endpoint = f"{address}:{endpoint_port}"
            endpoint_stats = capture.get("uplink" if direction == "up" else "downlink", {}).get("endpoints", {})
            flow_bound = bool(bound and address == expected_peer and 0 < int(endpoint_port) <= 65535
                              and int(epoch) == expected_epoch and endpoint in endpoint_stats
                              and (integer(endpoint_stats[endpoint].get("datagrams")) or 0) > 0)
            if direction == "up":
                # The uplink flow endpoint must be the publisher's own local UDP
                # port. A missing or zero port can no longer skip this binding.
                flow_bound = bool(flow_bound and local_port_valid and int(endpoint_port) == local_port)
        else:
            unconfirmed("independentMediaFlows.name:" + name)
        no_late = bool(valid and number(last) and number(read)
                       and (integer(flow.get("videoDatagrams")) or 0) > 0
                       and start <= last <= end and last - 1 <= read <= end
                       and max(last, read)-offset_ms+offset_error_ms <= stop)
        if not (number(last) and number(read)): unconfirmed("independentMediaFlows:%s clocks" % name)
        elif not no_late and valid: reject("media-flow-active-after-stop:" + name)
        flow_results.append({"flow": name, "lastKernelEpochMs": last, "lastReadEpochMs": read,
                             "bindingConfirmed": flow_bound, "noLateMedia": no_late})
    directions = {name.split(":", 1)[0] for name in flows}
    if not ({"up", "down"} <= directions): reject("media-flow-directions")
    events_valid = isinstance(capture.get("packetEvents"), list) and bool(capture.get("packetEvents"))
    if not events_valid: unconfirmed("capture.packetEvents")
    for event in capture.get("packetEvents") or []:
        kernel, read = event.get("epochMs"), event.get("readEpochMs")
        prefix = f'{event.get("direction")}:{event.get("endpoint")}:epoch={expected_epoch}'
        events_valid = bool(events_valid and valid and number(kernel) and number(read)
                            and prefix in flows and start <= kernel <= end and kernel-1 <= read <= end
                            and max(kernel, read)-offset_ms+offset_error_ms <= stop
                            and ("streamEpoch" not in event or integer(event["streamEpoch"]) == expected_epoch))
    if capture.get("packetEvents") and valid and not events_valid and "capture.packetEvents" not in missing:
        reject("packet-event-active-or-mismatched")
    media_silent = bool(coverage and complete and bound and kernel_time and registered and events_valid
                        and {"up", "down"} <= directions and flow_results
                        and all(f["noLateMedia"] and f["bindingConfirmed"] for f in flow_results))
    # "Active before the requested stop" is decided by the largest epochMs at
    # or before the request among samples that are strictly attributable to the
    # stopped stream. Array order must never let an older active sample stand in
    # for a newer stopped one, and a sample whose own identity is missing,
    # malformed, contradictory or belongs to another stream must never prove
    # the stopped stream was publishing.
    pre_stop_entries = []
    same_stream_pre_stop = []
    pre_stop_identity_known = bool(binding_ok and local_port_valid)
    for sample in publisher.get("publisherSamples", []) or []:
        if not isinstance(sample, dict):
            continue
        stamp = sample.get("epochMs")
        if not (number(stamp) and number(requested) and stamp <= requested):
            continue
        pipeline = sample.get("pipeline") if isinstance(sample.get("pipeline"), dict) else {}
        epoch_seen, port_seen, identity_problem = sample_identity(sample, pipeline)
        publishing = bool(sample.get("publisher") is True or pipeline.get("publisher") is True)
        outbound = integer(sample.get("outboundBytes", pipeline.get("outboundBytes", _MISSING)))
        queued = integer(sample.get("outboundQueued", pipeline.get("outboundQueued", _MISSING)))
        entry = {"epochMs": stamp, "identity": "unconfirmed", "publishEpoch": epoch_seen,
                 "localUdpPort": port_seen, "publisher": publishing,
                 "outboundBytes": outbound, "outboundQueued": queued}
        if identity_problem is not None:
            unconfirmed(identity_problem)
        elif not pre_stop_identity_known:
            # The stopped stream's own identity cannot be bound (already
            # reported as missing evidence), so no sample can be attributed.
            entry["identity"] = "unbound"
        elif epoch_seen == expected_epoch and port_seen == local_port:
            entry["identity"] = "same"
            same_stream_pre_stop.append((stamp, publishing))
        else:
            # A strictly valid other generation/port: recorded only. It
            # neither proves nor accuses the stopped stream by itself.
            entry["identity"] = "other"
        pre_stop_entries.append(entry)
    active = False
    active_stamp = None
    if same_stream_pre_stop:
        active_stamp = max(stamp for stamp, _ in same_stream_pre_stop)
        states = {publishing for stamp, publishing in same_stream_pre_stop
                  if stamp == active_stamp}
        if len(states) > 1:
            # Contradictory states at the same latest timestamp: array order
            # cannot decide which one is "the" pre-stop state, so the proof is
            # unconfirmed rather than rejected.
            unconfirmed("publisherSamples.ambiguous-pre-stop-state@%s" % active_stamp)
        else:
            active = bool(states == {True} and requested - active_stamp <= 2000)
            if not active:
                reject("publisher-not-active-before-stop")
            else:
                newer_other_activity = [e["epochMs"] for e in pre_stop_entries
                                        if e["identity"] == "other" and e["publisher"]
                                        and e["epochMs"] > active_stamp]
                if newer_other_activity:
                    # The newest evidence is another stream publishing after
                    # the stopped stream's last same-stream sample, so the
                    # stopped stream's last pre-stop state is not established.
                    # This is unconfirmed (not an accusation of either stream).
                    unconfirmed("publisherSamples.other-stream-activity-after-pre-stop-activity@%s"
                                % max(newer_other_activity))
    elif pre_stop_identity_known and not any(e["identity"] == "unconfirmed"
                                             for e in pre_stop_entries):
        reject("publisher-not-active-before-stop")
    aggregate = bool(coverage and no_up and no_down)
    if valid and not coverage: reject("capture-does-not-cover-silence-window")
    evidence_sound = not missing and not rejected
    proof = bool(aggregate and media_silent and active and client_stopped and evidence_sound)
    status = "confirmed" if proof else ("rejected" if rejected else "unconfirmed")
    # Echo every pre-stop sample that is not attributable to the stopped stream,
    # plus the newest same-stream sample that decided the state. Real captures
    # carry thousands of otherwise identical same-stream samples; keeping only
    # the deciding one and the anomalies keeps the audit readable without hiding
    # any sample that could change the verdict.
    pre_stop_counts = {name: sum(1 for e in pre_stop_entries if e["identity"] == name)
                       for name in ("same", "other", "unconfirmed", "unbound")}
    pre_stop_echo = [e for e in pre_stop_entries if e["identity"] != "same"]
    if active_stamp is not None:
        pre_stop_echo += [e for e in pre_stop_entries
                          if e["identity"] == "same" and e["epochMs"] == active_stamp]
        pre_stop_echo.sort(key=lambda e: (e["epochMs"], e["identity"] != "same"))
    return {"schemaVersion": 7, "status": status,
            "missingEvidence": missing, "rejectedEvidence": rejected,
            "postStopSamples": post_stop_samples,
            "preStopSamples": pre_stop_echo,
            "preStopSampleCounts": pre_stop_counts,
            "postStopActivityObserved": bool(post_stop_samples and any(s["active"] and s["identity"] != "other" for s in post_stop_samples)),
            "clientPostStopRoleDeclared": role_declared, "clientPostStopOutboundBytes": after.get("outboundBytes") if isinstance(after, dict) else None,
            "clientPostStopOutboundQueued": after.get("outboundQueued") if isinstance(after, dict) else None,
            "clientLocalUdpPort": local_port, "localUdpPortBound": local_port_valid,
            "offsetMs": offset_ms, "offsetErrorMs": offset_error_ms,
            "expectedPeer": expected_peer, "expectedPort": port, "expectedStreamEpoch": expected_epoch,
            "captureBindingConfirmed": bound, "kernelTimestampBasisConfirmed": kernel_time,
            "registrationConfirmed": registered, "packetEventsConfirmed": events_valid,
            "clientPostStopConfirmed": client_stopped,
            "stopRequestedEpochMs": requested, "stopCompletedEpochMs": stop,
            "captureStartServerEpochMs": start, "captureEndServerEpochMs": end,
            "captureCoversSilenceWindow": coverage, "silenceWindowMs": silence_ms,
            "observedAfterStopLowerMs": end - offset_ms - offset_error_ms - stop if valid else None,
            "lastUplinkLocalEpochMs": up, "lastDownlinkLocalEpochMs": down,
            "lastUplinkLatestPossibleLocalEpochMs": up + offset_error_ms if up is not None else None,
            "lastDownlinkLatestPossibleLocalEpochMs": down + offset_error_ms if down is not None else None,
            "uplinkCaptureEventRelation": relation(up), "downlinkCaptureEventRelation": relation(down),
            "timestampScope": capture.get("timestampBasis", "unspecified capture clock; no complete proof")
                + "; arrival/capture time alone does not prove when the client created a new send",
            "noLateUplink": no_up, "noLateDownlink": no_down, "lateBuckets": late,
            "aggregateSilenceOk": aggregate,
            "captureCompletenessConfirmed": complete, "kernelMediaCapture": packet_stats,
            "mediaFlowStop": flow_results, "mediaFlowSilenceConfirmed": media_silent,
            "publisherActiveBeforeRequestedStop": active,
            "publisherActiveSampleEpochMs": active_stamp,
            "completeMediaStopConfirmed": proof, "ok": proof}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("capture", "publisher", "out"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--offset-ms", type=float, required=True)
    p.add_argument("--offset-error-ms", type=float, default=150.)
    p.add_argument("--silence-ms", type=float, default=5000.)
    p.add_argument("--expected-peer")
    p.add_argument("--expected-port", type=int)
    a = p.parse_args()
    with open(a.capture, encoding="utf-8-sig") as f:
        capture = json.load(f)
    with open(a.publisher, encoding="utf-8-sig") as f:
        publisher = json.load(f)
    result = evaluate(capture, publisher, a.offset_ms, a.offset_error_ms, a.silence_ms,
                      a.expected_peer, a.expected_port)
    result.update({"capture": a.capture, "publisher": a.publisher})
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
