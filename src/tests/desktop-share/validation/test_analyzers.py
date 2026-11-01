import copy
import json
import tempfile
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from analyze_run import load_result
import unittest
from analyze_recovery import evaluate_viewer, viewer_samples, attach_outcome
from analyze_stop import evaluate


def samples(points):
    return [{"epochMs": t, "frames": n, "watching": True, "connected": True,
             "contentEpochMs": t - 50} for t, n in points]


def moving(start=2100, end=11000):
    return samples([(0, 0), (1900, 20), (2000, 20)] +
                   [(t, 20 + (t - start) // 100 + 1) for t in range(start, end + 1, 500)])


def observed(timeline, terminal_at=None):
    stream = dict(generation="1", sessionId="2", roomId="3", streamEpoch="4")
    for s in timeline:
        terminal = terminal_at is not None and s["epochMs"] >= terminal_at
        s["watching"] = not terminal
        s["recovery"] = dict(schemaVersion=1, stream=copy.deepcopy(stream),
                             state="terminal" if terminal else "active", failure={})
        if terminal:
            s["recovery"]["failure"] = dict(stream=copy.deepcopy(stream),
                reasonCode="UDP_ENDPOINT_UNAVAILABLE", message="UDP endpoint timed out",
                occurredEpochMs=terminal_at, visibleEpochMs=terminal_at)
    return timeline


class RecoveryTests(unittest.TestCase):
    def check_recovery(self, timeline, expected=False, **kwargs):
        result = evaluate_viewer(timeline, 1950, 2000, 0, 1000, **kwargs)
        self.assertEqual(result["recovered"], expected, result)
        return result

    def test_normal(self):
        r = self.check_recovery(moving(), True)
        self.assertGreaterEqual(r["resumeDelayUpperSeconds"], r["resumeDelayLowerSeconds"])

    def test_independent_burst_then_freeze(self):
        r = self.check_recovery(samples([(0, 0), (900, 10), (1900, 20), (2000, 20),
                                        (2100, 30), (2500, 30), (4500, 30), (10000, 30)]))
        self.assertFalse(r["sustainedFor2s"])
        self.assertTrue(r["frozenAfterFault"])

    def test_independent_margin_hides_5100ms(self):
        s = samples([(0, 0), (1500, 10), (2100, 20), (2150, 20), (6600, 20)] +
                    [(t, 21 + (t - 7100) // 100) for t in range(7100, 10101, 500)])
        r = evaluate_viewer(s, 1900, 2000, 150, 1000)
        self.assertFalse(r["resumedWithin5s"], r)
        self.assertGreaterEqual(r["resumeDelayUpperSeconds"], 5.1)
        self.assertFalse(r["recovered"])

    def test_fault_only_frames(self):
        self.check_recovery(samples([(0, 0), (900, 10), (1500, 20), (2500, 20), (5000, 20)]))

    def test_normal_then_late_freeze(self):
        s = moving(end=6100)
        s += samples([(7100, s[-1]["frames"]), (8100, s[-1]["frames"]), (10100, s[-1]["frames"])])
        r = self.check_recovery(s)
        self.assertTrue(r["sustainedFor2s"])
        self.assertFalse(r["updatesAtEnd"])

    def test_sparse_observation(self):
        self.check_recovery(samples([(1900, 20), (2200, 20), (2400, 21), (9000, 100)]))

    def test_guard_only_changes_confirmation(self):
        a = evaluate_viewer(moving(), 1950, 2000, 0)
        b = evaluate_viewer(moving(), 1950, 2000, 150)
        self.assertGreaterEqual(b["resumeDelayUpperSeconds"], a["resumeDelayUpperSeconds"])

    def test_repeated_old_picture_cannot_count_as_recovery(self):
        s = moving()
        for sample in s:
            sample["contentEpochMs"] = 1900
        self.check_recovery(s)

    def test_frame_counts_without_content_evidence_are_inconclusive(self):
        s = moving()
        for sample in s: del sample["contentEpochMs"]
        self.check_recovery(s)

    def test_dense_sampling_cannot_hide_burst_then_freeze(self):
        s = samples([(1900, 20), (2000, 20), (2100, 21), (2101, 30)] +
                    [(t, 30) for t in range(2102, 4202)])
        for sample in s[3:]: sample["contentEpochMs"] = 2051
        self.check_recovery(s)

    def test_readback_jitter_on_old_barcode_is_not_new_content(self):
        data = {"frameSamples": samples([(1900, 20), (2000, 20)] +
                                       [(t, 21 + i * 15) for i, t in enumerate(range(2100, 4601, 500))]),
                "latencySamples": []}
        for i, s in enumerate(data["frameSamples"]):
            source = 1800 if s["epochMs"] <= 2000 else 2050
            measured = s["epochMs"] + 10 - i
            s.pop("contentEpochMs")
            s["latencyMs"] = measured - source
            data["latencySamples"].append(dict(epochMs=measured, frames=s["frames"],
                                              latencyMs=measured-source, publisherEpochMs=source & 0xFFFFFF))
        paired = viewer_samples(data)
        self.assertEqual({s["contentEpochMs"] for s in paired[2:]}, {2050})
        self.check_recovery(paired)

    def test_source_timestamp_pairing_rejects_bad_code_and_unmatched_samples(self):
        s = samples([(2100, 20)])[0]
        good = dict(epochMs=2105, frames=20, latencyMs=100, publisherEpochMs=2005)
        self.assertEqual(viewer_samples({"frameSamples": [s], "latencySamples": [good]})[0]["contentEpochMs"], 2005)
        for bad in (dict(good, publisherEpochMs=1), dict(good, frames=19), dict(good, latencyMs=60001)):
            self.assertNotIn("contentEpochMs", viewer_samples({"frameSamples": [s], "latencySamples": [bad]})[0])

    def test_explicit_failure_is_timed_and_never_called_recovered(self):
        s = observed(samples([(0, 1), (900, 10), (2000, 20), (2500, 20), (6000, 20)]), 2500)
        r = evaluate_viewer(s, 1950, 2000, 150, 1000)
        attach_outcome(r, {"frameSamples": s, "error": "unrelated test assertion"}, 1950, 1000)
        self.assertTrue(r["explicitFailureWithin5s"])
        self.assertFalse(r["recovered"])
        self.assertEqual(r["explicitFailureDelayLowerSeconds"], .5)
        self.assertEqual(r["explicitFailureDelayUpperSeconds"], .55)
        attach_outcome(r, {"frameSamples": s}, 1950, 1000)
        self.assertTrue(r["explicitFailureWithin5s"])
        for t in (observed(samples([(0, 1), (900, 10), (2000, 20), (8000, 20)]), 8000),
                  samples([(0, 1), (900, 10), (2000, 20), (2500, 20)])):
            m = evaluate_viewer(t, 1950, 2000, 150, 1000)
            attach_outcome(m, {"frameSamples": t, "error": "failure"}, 1950, 1000)
            self.assertFalse(m["explicitFailureWithin5s"], m)

    def test_preparing_cannot_hide_final_frozen_active_stream(self):
        s = observed(samples([(0, 1), (900, 10), (2000, 20), (2500, 20), (3000, 20), (5000, 20), (10000, 20)]))
        s[3].update(watching=False, status="preparing receiver")
        s[3]["recovery"]["state"] = "preparing"
        for t in s[2:]: t["contentEpochMs"] = 1800
        r = evaluate_viewer(s, 1950, 2000, 150, 1000)
        attach_outcome(r, dict(frameSamples=s, error="no continuing decoded frames"), 1950, 1000)
        self.assertTrue(r["frozenAfterFault"])
        self.assertTrue(r["stillWatchingAtEnd"])
        self.assertEqual(r["outcome"], "unconfirmed")

    def test_preparing_cannot_backdate_late_terminal(self):
        s = observed(samples([(0, 1), (900, 10), (2000, 20), (2500, 20), (5000, 20), (7100, 20), (10000, 20)]), 7100)
        for t in s[3:5]:
            t.update(watching=False, status="preparing receiver")
            t["recovery"]["state"] = "preparing"
        r = evaluate_viewer(s, 1950, 2000, 150, 1000)
        attach_outcome(r, dict(frameSamples=s), 1950, 1000)
        self.assertEqual(r["explicitFailureDelayLowerSeconds"], 5.1)
        self.assertEqual(r["explicitFailureDelayUpperSeconds"], 5.15)
        self.assertFalse(r["explicitFailureWithin5s"])
        self.assertTrue(r["terminalStateConfirmedAtEnd"])

    def test_terminal_event_must_match_stream_reason_time_and_final_state(self):
        base = observed(samples([(0, 1), (900, 10), (2000, 20), (2500, 20), (6000, 20)]), 2500)
        mutations = [
            lambda t: t[-1].update(watching=True),
            lambda t: t[-1]["recovery"].update(state="preparing"),
            lambda t: t[-1]["recovery"]["stream"].update(generation="2"),
            lambda t: t[3]["recovery"]["failure"]["stream"].update(streamEpoch="5"),
            lambda t: t[3]["recovery"]["failure"].update(reasonCode="preparing receiver"),
            lambda t: t[3]["recovery"]["failure"].update(occurredEpochMs=500),
            lambda t: t[3]["recovery"]["failure"].update(visibleEpochMs=8000),
            lambda t: t[-1]["recovery"].update(failure={}),
            lambda t: t[2].pop("recovery"),
        ]
        for mutate in mutations:
            t = copy.deepcopy(base); mutate(t)
            r = evaluate_viewer(t, 1950, 2000, 150, 1000)
            attach_outcome(r, dict(frameSamples=t, error="test failed"), 1950, 1000)
            self.assertFalse(r["explicitFailureWithin5s"], r)

    def test_terminal_visibility_deadline_cannot_use_earlier_failure_decision(self):
        s = observed(samples([(0, 1), (900, 10), (2000, 20), (7100, 20), (10000, 20)]), 7100)
        for t in s[3:]: t["recovery"]["failure"]["occurredEpochMs"] = 2500
        r = evaluate_viewer(s, 1950, 2000, 150, 1000)
        attach_outcome(r, dict(frameSamples=s), 1950, 1000)
        self.assertFalse(r["explicitFailureWithin5s"])
        self.assertEqual(r["explicitFailureDelayUpperSeconds"], 5.15)

    def test_backward_or_reset_timeline_rejected(self):
        for bad in (samples([(2000, 20), (1000, 30)]) + moving(),
                    moving() + samples([(12000, 1)])):
            self.assertFalse(self.check_recovery(bad)["timelineValid"])

    def test_duplicate_snapshot_adds_no_recovery_evidence(self):
        s = moving()
        s.append(dict(s[-1]))
        self.assertEqual(self.check_recovery(s, True)["identicalSnapshotsCollapsed"], 1)
        s[-1]["frames"] += 1
        self.assertFalse(self.check_recovery(s)["timelineValid"])

    def test_final_snapshot_within_one_frame_is_not_a_freeze(self):
        s = moving()
        last = dict(s[-1]); last["epochMs"] += 1
        s.append(last)
        self.check_recovery(s, True)

    def test_deadline_uncertain_does_not_pass(self):
        s = samples([(1900, 20), (2150, 20), (6600, 20)] +
                    [(t, 21 + i * 15) for i, t in enumerate(range(7000, 11001, 500))])
        r = evaluate_viewer(s, 1900, 2000, 150)
        self.assertTrue(r["deadlineUncertain"])
        self.assertFalse(r["recovered"])

    def test_static_needs_new_content_challenge(self):
        s = samples([(1900, 20), (2200, 20), (3000, 21), (4000, 21), (6000, 21)])
        self.check_recovery(s)
        proof = {"challengeInstalledEpochMs": 2300, "decodedMarkerVerifiedEpochMs": 3000,
                 "uniqueNewSourceMarker": True, "expectedMarker": "new-unique-B",
                 "decodedMarker": "new-unique-B", "decodedImageSha256": "a" * 64,
                 "decodedMarkerAtEnd": "new-unique-B", "endImageSha256": "a" * 64,
                 "endVerifiedEpochMs": 6000}
        self.assertFalse(self.check_recovery(s, True, static_proof=proof)["frozenAfterFault"])
        for key, value in (("challengeInstalledEpochMs", 1800), ("decodedMarker", "old-A"),
                           ("decodedMarkerVerifiedEpochMs", 7100), ("decodedImageSha256", ""),
                           ("decodedMarkerAtEnd", "old-A"), ("endVerifiedEpochMs", 3100)):
            p = dict(proof, **{key: value})
            self.check_recovery(s, static_proof=p)


class ResultIntegrityTests(unittest.TestCase):
    def test_metric_snapshot_alone_cannot_hide_unload_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            raw = Path(folder)/'viewer1.json'
            raw.write_text(json.dumps(dict(ok=True,unloadPending=True,unloadRecord='unload-viewer1.json')),encoding='utf-8')
            self.assertFalse(load_result(str(raw))['ok'])
            record=Path(folder)/'unload-viewer1.json'
            (Path(folder)/'run-summary.json').write_text(json.dumps(dict(viewerResults=[dict(exitCode=0)])),encoding='utf-8')
            for good,committed,code,expected in ((False,True,1,False),(True,False,1,False),(True,True,1,False),(True,True,0,True)):
                record.write_text(json.dumps(dict(unloadOk=good,rawResultCommitted=committed,finalExitCode=code)),encoding='utf-8')
                self.assertEqual(load_result(str(raw))['ok'],expected)

    def test_successful_unload_record_cannot_hide_actual_process_crash(self):
        with tempfile.TemporaryDirectory() as folder:
            raw=Path(folder)/'viewer1.json'
            raw.write_text(json.dumps(dict(ok=True,unloadPending=True,unloadRecord='unload-viewer1.json')))
            (Path(folder)/'unload-viewer1.json').write_text(json.dumps(dict(unloadOk=True,rawResultCommitted=True,finalExitCode=0)))
            self.assertFalse(load_result(str(raw))['ok'])
            for code, expected in ((-1073741819,False),(1,False),(0,True)):
                (Path(folder)/'run-summary.json').write_text(json.dumps(dict(viewerResults=[dict(exitCode=code)])))
                self.assertEqual(load_result(str(raw))['ok'],expected)


class StopTests(unittest.TestCase):
    def setUp(self):
        self.capture = {"startedAtEpochMs": 1000, "endedAtEpochMs": 11000,
                        "lastUplinkEpochMs": 3000, "lastDownlinkEpochMs": 3100, "buckets": [],
                        "kernelPacketStatistics": {"media": {"kernelPackets": 2, "kernelDrops": 0}},
                        "independentMediaFlows": {
                            "up:client:epoch=1": dict(lastKernelEpochMs=3000, videoDatagrams=1),
                            "down:client:epoch=1": dict(lastKernelEpochMs=3100, videoDatagrams=1)}}
        self.publisher = {"stopRequestedEpochMs": 3000, "stopCompletedEpochMs": 4000,
                          "publisherSamples": [dict(epochMs=2900, pipeline=dict(
                              publisher=True, publishEpoch="1", localUdpPort=50001))]}
        self.capture.update(ztPeer="10.10.10.3", port=18500, packetEvents=[],
            timestampBasis="SO_TIMESTAMPNS kernel packet timestamps; read epochs separate",
            uplink=dict(registerDatagrams=2,endpoints={"10.10.10.3:50001":dict(datagrams=2)}),
            downlink=dict(registerAckDatagrams=2,endpoints={"10.10.10.3:50002":dict(datagrams=2)}),
            independentMediaFlows={
                "up:10.10.10.3:50001:epoch=1":dict(lastKernelEpochMs=3000,lastReadEpochMs=3001,videoDatagrams=1),
                "down:10.10.10.3:50002:epoch=1":dict(lastKernelEpochMs=3100,lastReadEpochMs=3101,videoDatagrams=1)})
        self.publisher.update(stats=dict(localAddress="10.10.10.3",serverUdpPort=18500,publishEpoch="1",
                                         localUdpPort=50001),
                              statsAfterStop=dict(outboundBytes=0,outboundQueued=0,publisher=False))
        self.capture['packetEvents']=[dict(epochMs=3000,readEpochMs=3001,direction='up',endpoint='10.10.10.3:50001'),
                                      dict(epochMs=3100,readEpochMs=3101,direction='down',endpoint='10.10.10.3:50002')]

    def test_valid_coverage(self):
        self.assertTrue(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_aggregate_silence_cannot_hide_capture_loss_or_wrong_binding(self):
        for key, value in (("kernelPacketStatistics", {"media": {"kernelPackets": 50, "kernelDrops": 11}}),
                           ("kernelPacketStatistics", {}), ("independentMediaFlows", {})):
            c = copy.deepcopy(self.capture); c[key] = value
            r = evaluate(c, self.publisher, 0)
            self.assertTrue(r["aggregateSilenceOk"])
            self.assertFalse(r["ok"])

    def test_already_inactive_publisher_does_not_prove_active_stream_stop(self):
        self.publisher["publisherSamples"][-1]["pipeline"]["publisher"] = False
        self.assertFalse(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_each_media_flow_is_checked(self):
        self.capture["independentMediaFlows"]["down:second-client:epoch=2"] = dict(lastKernelEpochMs=4100, videoDatagrams=1)
        self.assertFalse(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_capture_ends_early(self):
        self.capture["endedAtEpochMs"] = 5000
        self.assertFalse(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_capture_starts_late(self):
        self.capture["startedAtEpochMs"] = 5000
        self.assertFalse(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_uplink_after_stop(self):
        self.capture["lastUplinkEpochMs"] = 6000
        r = evaluate(self.capture, self.publisher, 0)
        self.assertFalse(r["noLateUplink"])
        self.assertFalse(r["ok"])

    def test_clock_uncertainty_cannot_hide_a_late_packet(self):
        self.capture["lastUplinkEpochMs"] = 4100
        self.assertFalse(evaluate(self.capture, self.publisher, 0, 150)["ok"])
        self.capture["lastUplinkEpochMs"] = 3900
        self.assertFalse(evaluate(self.capture, self.publisher, 0, 150)["ok"])
        self.assertTrue(evaluate(self.capture, self.publisher, 0, 20)["ok"])

    def test_downlink_after_stop(self):
        self.capture["lastDownlinkEpochMs"] = 6000
        self.assertFalse(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_clock_bound_affects_coverage(self):
        self.capture["endedAtEpochMs"] = 9100
        self.assertFalse(evaluate(self.capture, self.publisher, 0, 150)["ok"])
        self.assertTrue(evaluate(self.capture, self.publisher, 0, 0)["ok"])

    def test_bucket_disagreement_rejected(self):
        for key in ("upDatagrams", "downDatagrams"):
            self.capture["buckets"] = [{"startOffsetSeconds": 5, key: 1}]
            self.assertFalse(evaluate(self.capture, self.publisher, 0)["ok"])

    def test_empty_or_missing_evidence_rejected(self):
        for key in ("endedAtEpochMs", "startedAtEpochMs", "lastUplinkEpochMs", "lastDownlinkEpochMs"):
            c = copy.deepcopy(self.capture)
            del c[key]
            self.assertFalse(evaluate(c, self.publisher, 0)["ok"])

    def test_requested_silence_duration_is_used(self):
        self.assertTrue(evaluate(self.capture, self.publisher, 0, silence_ms=5000)["ok"])
        self.assertFalse(evaluate(self.capture, self.publisher, 0, silence_ms=10000)["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
