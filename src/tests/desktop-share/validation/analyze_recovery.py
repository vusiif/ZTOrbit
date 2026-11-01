#!/usr/bin/env python3
"""Recovery schema v3. All times use the viewer/driver clock.

Removal occurs in [clearSent, clearReturned]. A baseline at/after returned+guard
excludes uncertain content, but the guard NEVER moves the recovery stopwatch.
The recovery upper bound is newSample-clearSent; only <=5s passes. The lower
bound uses the last observed pre-clear source content, or zero if earlier
recovery cannot be excluded. The adjacent confirmation observation interval is
reported separately (the guarded baseline may already contain recovered frames).
Continuous updates must cover both the first and final
2s windows, with sample gaps <=1.25s. Sparse evidence is inconclusive/fails.
Static recovery needs an independently verified new post-clear source marker;
unchanged frame counts or repeated display of old content cannot prove recovery.
"""
from __future__ import annotations
import argparse
import glob
import json
import os


def load(path):
    with open(path, encoding="utf-8-sig") as handle:
        return json.load(handle)


def frames_in(samples, start, end):
    prior = [s for s in samples if s["epochMs"] <= start]
    window = [s for s in samples if start <= s["epochMs"] <= end]
    return max(0, window[-1]["frames"] - (prior[-1] if prior else window[0])["frames"]) if window else 0


def viewer_samples(data):
    """Pair counters with exact decoded barcode observations on the same clock.

    frameSamples.epochMs precedes preview readback; latencySamples.epochMs is
    recorded after it. Subtracting latency from the earlier epoch would turn
    variable readback cost into apparent source updates on an old image.
    """
    measurements = data.get("latencySamples") or []
    result = []
    for raw in data.get("frameSamples") or []:
        sample = dict(raw)
        sample.pop("contentEpochMs", None)
        candidates = [m for m in measurements if m.get("frames") == sample["frames"]
                      and 0 <= m["epochMs"] - sample["epochMs"] <= 250
                      and 0 <= m.get("latencyMs", -1) <= 60000
                      and (int(m["epochMs"] - m["latencyMs"]) & 0xFFFFFF) == m.get("publisherEpochMs")]
        if candidates:
            m = min(candidates, key=lambda x: x["epochMs"] - sample["epochMs"])
            sample["counterEpochMs"] = sample["epochMs"]
            sample["epochMs"] = m["epochMs"]  # completion of visible content measurement
            sample["contentEpochMs"] = m["epochMs"] - m["latencyMs"]
            sample["contentMeasurementLagMs"] = m["epochMs"] - sample["counterEpochMs"]
        result.append(sample)
    return result


def continuous(window, duration, min_frames, max_gap):
    # Coalesce sub-frame snapshots into meaningful observation intervals. Merely
    # allowing every <100ms edge would let a dense burst-then-freeze timeline
    # evade the sustained-update check indefinitely.
    checks = []
    for s in window:
        if not checks or s["epochMs"] - checks[-1]["epochMs"] >= 100:
            checks.append(s)
    if len(checks) > 1 and window[-1]["epochMs"] > checks[-1]["epochMs"]:
        checks[-1] = window[-1]
    return (len(checks) >= 3 and checks[-1]["epochMs"] - checks[0]["epochMs"] >= duration
            and checks[-1]["frames"] - checks[0]["frames"] >= min_frames
            and all(s.get("watching") and s.get("connected") for s in window)
            and all(0 < b["epochMs"] - a["epochMs"] <= max_gap
                    and b["frames"] > a["frames"]
                    and a.get("contentEpochMs") is not None and b.get("contentEpochMs") is not None
                    and b["contentEpochMs"] > a["contentEpochMs"]
                    and a.get("watching") and a.get("connected")
                    and b.get("watching") and b.get("connected")
                    for a, b in zip(checks, checks[1:])))


def evaluate_viewer(samples, clear_sent, clear_returned, clock_error_ms,
                    fault_start=None, sustained_ms=2000., sustained_frames=5,
                    max_sample_gap_ms=1250., static_proof=None):
    # The harness may take the final snapshot twice in the same millisecond.
    # Collapse identical evidence; it proves neither elapsed time nor updates.
    # Conflicting counters/states at the same epoch still invalidate the input.
    original_count = len(samples)
    normalized = []
    for original in samples:
        sample = dict(original)
        keys = ("epochMs", "frames", "watching", "connected")
        if normalized and all(sample.get(k) == normalized[-1].get(k) for k in keys):
            continue
        normalized.append(sample)
    samples = normalized
    valid = (clear_sent <= clear_returned and clock_error_ms >= 0 and len(samples) >= 2
             and all(b["epochMs"] > a["epochMs"] and b["frames"] >= a["frames"]
                     for a, b in zip(samples, samples[1:])))
    threshold = clear_returned + clock_error_ms
    baseline = next((s for s in samples if s["epochMs"] >= threshold), None)
    after = [s for s in samples if baseline and s["epochMs"] > baseline["epochMs"]]
    floor = baseline["frames"] if baseline else None
    resume = next((s for s in after if s["frames"] > floor
                   and s.get("contentEpochMs", float('-inf')) > threshold), None)
    previous = next((a for a, b in zip(samples, samples[1:]) if b is resume), None)
    old_content = [s for s in samples if resume and s["epochMs"] < resume["epochMs"]
                   and s.get("contentEpochMs", float('inf')) <= clear_sent]
    lower = max(0., old_content[-1]["epochMs"] - clear_returned) if old_content else (0. if resume else None)
    upper = max(0., resume["epochMs"] - clear_sent) if resume else None
    recovery_window = []
    if resume:
        for s in after:
            if s["epochMs"] >= resume["epochMs"]:
                recovery_window.append(s)
                if s["epochMs"] >= resume["epochMs"] + sustained_ms:
                    break
    tail = []
    if after:
        boundary = after[-1]["epochMs"] - sustained_ms
        start = max((i for i, s in enumerate(after) if s["epochMs"] <= boundary), default=0)
        tail = after[start:]
    sustained = continuous(recovery_window, sustained_ms, sustained_frames, max_sample_gap_ms)
    tail_updates = continuous(tail, sustained_ms, sustained_frames, max_sample_gap_ms)
    final = samples[-1] if samples else {}
    still_watching = bool(final.get("watching") and final.get("connected"))
    static_ok = False
    if static_proof:
        installed = static_proof.get("challengeInstalledEpochMs", 0)
        verified = static_proof.get("decodedMarkerVerifiedEpochMs", 0)
        end_verified = static_proof.get("endVerifiedEpochMs", 0)
        static_ok = bool(static_proof.get("uniqueNewSourceMarker")
                         and static_proof.get("expectedMarker") == static_proof.get("decodedMarker")
                         and static_proof.get("expectedMarker")
                         and static_proof.get("decodedImageSha256")
                         and static_proof.get("endImageSha256")
                         and static_proof.get("decodedMarkerAtEnd") == static_proof.get("expectedMarker")
                         and installed > threshold and installed <= verified <= clear_sent + 5000
                         and verified + sustained_ms <= end_verified <= final.get("epochMs", 0)
                         and all(s.get("watching") and s.get("connected") for s in samples
                                 if verified <= s["epochMs"] <= end_verified)
                         and still_watching)
    result = {
        "schemaVersion": 3, "timelineValid": valid,
        "identicalSnapshotsCollapsed": original_count - len(samples),
        "clearSentEpochMs": clear_sent, "clearReturnedEpochMs": clear_returned,
        "clearCommandUncertaintyMs": clear_returned - clear_sent,
        "confirmationGuardMs": clock_error_ms, "newContentThresholdEpochMs": threshold,
        "baselineSampleEpochMs": baseline["epochMs"] if baseline else None,
        "framesAtBaseline": floor,
        "framesDuringFault": frames_in(samples, fault_start, clear_sent) if fault_start is not None else None,
        "framesNearClear": frames_in(samples, clear_sent, threshold),
        "framesAfter": final.get("frames", 0) - floor if floor is not None else None,
        "finalFrames": final.get("frames"),
        "firstConfirmedNewContentEpochMs": resume["epochMs"] if resume else None,
        "resumeDelayLowerSeconds": lower / 1000 if lower is not None else None,
        "resumeDelayUpperSeconds": upper / 1000 if upper is not None else None,
        "resumeDelaySeconds": upper / 1000 if upper is not None else None,
        "measurementIntervalMs": upper - lower if upper is not None else None,
        "confirmationObservationIntervalEpochMs": [previous["epochMs"], resume["epochMs"]] if previous else None,
        "minimumObservationStepMs": 100,
        "resumedWithin5s": upper is not None and upper <= 5000,
        "deadlineUncertain": lower is not None and lower <= 5000 < upper,
        "maxAllowedSampleGapMs": max_sample_gap_ms,
        "sustainedFor2s": sustained, "updatesAtEnd": tail_updates,
        "recoveryWindowSamples": recovery_window, "tailWindowSamples": tail,
        "frozenAfterFault": bool(still_watching and not static_ok and len(tail) >= 2
                                  and tail[-1]["epochMs"] - tail[0]["epochMs"] >= sustained_ms
                                  and (tail[-1]["frames"] == tail[0]["frames"]
                                       or (tail[0].get("contentEpochMs") is not None
                                           and tail[-1].get("contentEpochMs") == tail[0]["contentEpochMs"]))),
        "continuousRecoveryUnconfirmed": not (sustained and tail_updates),
        "stillWatchingAtEnd": still_watching,
        "contentProof": "decoded source timestamp must advance in every dynamic observation interval",
        "contentProofMissingSampleEpochMs": [s["epochMs"] for s in samples if s.get("contentEpochMs") is None],
        "staticChallengeConfirmed": static_ok,
        "criterion": "static-content-challenge" if static_proof else "dynamic-continuous-updates",
    }
    result["recovered"] = bool(valid and still_watching and (static_ok if static_proof else
                              result["resumedWithin5s"] and sustained and tail_updates))
    return result


TERMINAL_REASONS = frozenset({
    "CONTROL_CONNECTION_LOST", "ROOM_CLOSED", "UDP_ENDPOINT_UNAVAILABLE",
    "RECEIVER_CREATE_FAILED", "RECEIVER_START_FAILED", "MEDIA_CHANNEL_FAILED",
    "RECEIVER_SUBMIT_FAILED", "FIRST_FRAME_TIMEOUT", "MEDIA_SERVICE_FAILED",
    "STREAM_READY_FAILED",
})


def stream_identity(observation):
    stream = observation.get("stream")
    keys = ("generation", "sessionId", "roomId", "streamEpoch")
    if not isinstance(stream, dict) or not all(
            isinstance(stream.get(k), str) and stream[k].isdigit() and int(stream[k]) > 0 for k in keys):
        return None
    return tuple(stream[k] for k in keys)


def attach_outcome(metrics, data, clear_sent, fault_start=None):
    """Use controller events; harness assertions and display text are not evidence.

    Old runs without structured observations remain unconfirmed in this branch.
    Recovery still uses their independently measured source content as before.
    """
    start = fault_start if fault_start is not None else clear_sent
    samples = data.get("frameSamples") or []
    baselines = [s for s in samples if s["epochMs"] < start and s["frames"] > 0
                 and s.get("watching") and s.get("connected")]
    baseline = baselines[-1] if baselines else None
    identity = stream_identity(baseline.get("recovery", {})) if baseline else None
    terminal = None
    failure = None
    confirmed = False
    after = [s for s in samples if baseline and s["epochMs"] >= baseline["epochMs"]]
    same_stream = bool(identity and all(s.get("recovery", {}).get("schemaVersion") == 1
                       and stream_identity(s.get("recovery", {})) == identity for s in after))
    if any("recovery" in s for s in samples) and not same_stream:
        metrics["recovered"] = False
    if same_stream:
        for i, s in enumerate(after):
            o = s["recovery"]
            f = o.get("failure", {})
            if o.get("state") != "terminal":
                continue
            occurred, visible = f.get("occurredEpochMs"), f.get("visibleEpochMs")
            if (s.get("watching") is not False
                    or f.get("reasonCode") not in TERMINAL_REASONS
                    or stream_identity(f) != identity or not f.get("message")
                    or not isinstance(occurred, (int, float)) or not isinstance(visible, (int, float))
                    or not start <= occurred <= visible <= s["epochMs"]
                    or (i > 0 and after[i-1]["epochMs"] > visible)):
                break
            terminal, failure = s, f
            # Every later observation, including the final one, must retain the
            # same terminal event. Preparing, retrying or an active frozen stream
            # invalidates it even if an earlier snapshot really was a failure.
            confirmed = all(t.get("watching") is False
                            and t["recovery"].get("state") == "terminal"
                            and t["recovery"].get("failure") == f for t in after[i:])
            break
    upper = max(0., failure["visibleEpochMs"] - clear_sent) / 1000 if failure else None
    lower = max(0., failure["visibleEpochMs"] - metrics["clearReturnedEpochMs"]) / 1000 if failure else None
    explicit = bool(metrics["timelineValid"] and baseline and confirmed and upper <= 5.)
    metrics.update({"activeBeforeFault": bool(baseline),
                    "terminalObservationSchemaVersion": 1,
                    "terminalStreamConsistent": same_stream,
                    "terminalStateConfirmedAtEnd": confirmed,
                    "firstTerminalSample": terminal,
                    "terminalFailureEvent": failure,
                    "explicitFailureDelayLowerSeconds": lower,
                    "explicitFailureDelayUpperSeconds": upper,
                    "explicitFailureDeadlineUncertain": lower is not None and lower <= 5 < upper,
                    "explicitFailureWithin5s": explicit,
                    "meetsRecoveryOrExplicitFailureWithin5s": metrics["recovered"] or explicit,
                    "outcome": "recovered" if metrics["recovered"] else "explicit-failure" if explicit else "unconfirmed"})
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir")
    parser.add_argument("--clear-sent-epoch-ms", type=float, required=True)
    parser.add_argument("--clear-returned-epoch-ms", type=float, required=True)
    parser.add_argument("--clock-error-ms", type=float, default=150., help="content confirmation guard; never deducted from elapsed time")
    parser.add_argument("--fault-start-epoch-ms", type=float)
    parser.add_argument("--static-proof", help="per-viewer content challenge proofs, keyed by viewer filename")
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    proofs = load(args.static_proof) if args.static_proof else {}
    result = {"schemaVersion": 3, "runDir": os.path.abspath(args.run_dir), "viewers": []}
    for path in sorted(glob.glob(os.path.join(args.run_dir, "viewer*.json"))):
        data = load(path)
        name = os.path.basename(path)
        metrics = evaluate_viewer(viewer_samples(data), args.clear_sent_epoch_ms,
                                  args.clear_returned_epoch_ms, args.clock_error_ms,
                                  args.fault_start_epoch_ms, static_proof=proofs.get(name))
        metrics.update({"file": name, "ok": data.get("ok"), "error": data.get("error"),
                        "decodedFrames": data.get("decodedFrames"), "stats": data.get("stats")})
        attach_outcome(metrics, data, args.clear_sent_epoch_ms, args.fault_start_epoch_ms)
        result["viewers"].append(metrics)
    result["allRecovered"] = bool(result["viewers"]) and all(v["recovered"] for v in result["viewers"])
    result["allResolvedWithin5s"] = bool(result["viewers"]) and all(v["meetsRecoveryOrExplicitFailureWithin5s"] for v in result["viewers"])
    payload = json.dumps(result, indent=2, ensure_ascii=False)
    with open(args.out or os.path.join(args.run_dir, "fault-analysis.json"), "w", encoding="utf-8") as handle:
        handle.write(payload)
    print(payload)
    return 0 if result["allRecovered"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
