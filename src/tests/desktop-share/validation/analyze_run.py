#!/usr/bin/env python3
"""Summarise one stage-7 run directory (publisher + N viewers) into JSON.

Reads only the JSON/log artefacts produced by run-one.ps1 and the client E2E
harness. Computes frame rate, latency percentiles, queue figures and resource
peaks without re-interpreting the protocol.
"""

from __future__ import annotations

import argparse
import glob
import json
import os


def load(path: str):
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def load_result(path: str):
    data = load(path)
    if data.get("unloadPending"):
        record = os.path.join(os.path.dirname(path), os.path.basename(data.get("unloadRecord", "")))
        outcome = load(record) if os.path.isfile(record) else {}
        data["unloadOutcome"] = outcome
        data["okBeforeUnload"] = data.get("ok")
        data["ok"] = bool(data.get("ok") and outcome.get("unloadOk") and outcome.get("rawResultCommitted")
                          and outcome.get("finalExitCode") == 0)
        data["unloadRecordedExitCode"] = outcome.get("finalExitCode")
        summary_path = os.path.join(os.path.dirname(path), "run-summary.json")
        summary = load(summary_path) if os.path.isfile(summary_path) else {}
        stem = os.path.splitext(os.path.basename(path))[0]
        actual = None
        if stem == "publisher":
            actual = summary.get("publisherExitCode")
        elif stem.startswith("viewer") and stem[6:].isdigit():
            index = int(stem[6:])-1
            results = summary.get("viewerResults", [])
            if 0 <= index < len(results): actual = results[index].get("exitCode")
        data["actualProcessExitCode"] = actual
        data["processExitConfirmed"] = actual == 0
        data["ok"] = bool(data["ok"] and actual == 0)
    return data


def percentile(values, fraction: float):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def summarise_viewer(path: str) -> dict:
    data = load_result(path)
    stats = data.get("stats", {})
    samples = data.get("frameSamples") or []
    fps = None
    window_s = None
    window_frames = None
    if len(samples) >= 2:
        window_s = (samples[-1]["epochMs"] - samples[0]["epochMs"]) / 1000.0
        window_frames = samples[-1]["frames"] - samples[0]["frames"]
        if window_s > 0:
            fps = window_frames / window_s
    latency = sorted(entry["latencyMs"] for entry in (data.get("latencySamples") or []))
    first_delay = data.get("firstFrameDelayMs")
    first_snapshot = data.get("statsAfterFirstFrameWait", {})
    first_proven = (isinstance(first_delay, (int, float)) and 0 <= first_delay < 30000
                    and first_snapshot.get("decodedFrames", 0) > 0)
    return {
        "file": os.path.basename(path),
        "ok": data.get("ok"),
        "error": data.get("error"),
        "exitCode": data.get("exitCode"),
        "unloadOutcome": data.get("unloadOutcome"),
        "unloadRecordedExitCode": data.get("unloadRecordedExitCode"),
        "actualProcessExitCode": data.get("actualProcessExitCode"),
        "processExitConfirmed": data.get("processExitConfirmed", False),
        "decodedFrames": data.get("decodedFrames"),
        "frameWindowSeconds": round(window_s, 2) if window_s else None,
        "frameWindowFrames": window_frames,
        "averageFps": round(fps, 2) if fps else None,
        "averageFpsExact": fps,
        "frameWindowSecondsExact": window_s,
        "firstFrameDelayMs": first_delay if first_proven else None,
        "claimedFirstFrameDelayMs": first_delay,
        "firstFrameDecodedProof": first_proven,
        "readyDelayMs": data.get("readyDelayMs"),
        "latencySamples": len(latency),
        "frameSampleCount": len(samples),
        "latencyMissingFrameSamples": [s["epochMs"] for s in samples if "latencyMs" not in s],
        "latencyKnownFraction": sum("latencyMs" in s for s in samples)/len(samples) if samples else None,
        "fractionAllObservationsWithin500ms": sum(0 <= s.get("latencyMs", float('inf')) <= 500 for s in samples)/len(samples) if samples else None,
        "latencyMissingReason": "EXE does not encode rejection reasons; missing barcode/out-of-range (>60s) observations are listed, never counted as zero latency. Static tests disable the latency probe.",
        "latencyP50Ms": round(percentile(latency, 0.50), 1) if latency else None,
        "latencyP95Ms": round(percentile(latency, 0.95), 1) if latency else None,
        "latencyMaxMs": round(latency[-1], 1) if latency else None,
        "completePackets": stats.get("completePackets"),
        "submittedPackets": stats.get("submittedPackets"),
        "inboundDatagrams": stats.get("inboundDatagrams"),
        "gapsDetected": stats.get("gapsDetected"),
        "duplicateDatagrams": stats.get("duplicateDatagrams"),
        "expiredFragments": stats.get("expiredFragments"),
        "droppedUntilKeyFrame": stats.get("droppedUntilKeyFrame"),
        "pendingPackets": stats.get("pendingPackets"),
        "bufferedBytes": stats.get("bufferedBytes"),
        "inboundDroppedBudget": stats.get("inboundDroppedBudget"),
        "peakPendingPackets": stats.get("peakPendingPackets"),
        "peakBufferedBytes": stats.get("peakBufferedBytes"),
        "postLeaveQueued": (data.get("statsAfterLeave") or {}).get("inboundQueued"),
        "postLeavePending": (data.get("statsAfterLeave") or {}).get("pendingPackets"),
        "datagramsSent": stats.get("datagramsSent"),
        "screenshot": data.get("screenshot"),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir")
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    run_dir = args.run_dir
    result = {"runDir": os.path.abspath(run_dir), "viewers": []}
    summary_path = os.path.join(run_dir, "run-summary.json")
    if os.path.exists(summary_path):
        run = load(summary_path)
        result.update({
            "scenario": run.get("scenario"),
            "serverAddress": run.get("server"),
            "profile": run.get("profile"),
            "codec": run.get("codec"),
            "viewersRequested": run.get("viewers"),
            "durationMs": run.get("durationMs"),
            "startedAt": run.get("startedAt"),
            "finishedAt": run.get("finishedAt"),
            "elapsedSeconds": run.get("elapsedSeconds"),
            "publisherExitCode": run.get("publisherExitCode"),
            "viewerExitCodes": [entry.get("exitCode") for entry in (run.get("viewerResults") or [])],
            "switchAtMs": run.get("switchAtMs"),
            "switchTag": run.get("switchTag"),
            "staticSource": run.get("staticSource"),
            "stallFirstViewerMs": run.get("stallFirstViewerMs"),
            "shortViewerDurationMs": run.get("shortViewerDurationMs"),
            "expectJoinFailLastViewer": run.get("expectJoinFailLastViewer"),
        })
        resources = run.get("resourceSamples") or []
        if resources:
            result["resources"] = {
                "samples": len(resources),
                "peakRssMiB": max(row.get("rssMiB", 0) for row in resources),
                "peakHandles": max(row.get("handles", 0) for row in resources),
                "peakThreads": max(row.get("threads", 0) for row in resources),
                "peakCpuSeconds": max(row.get("cpuSeconds", 0) for row in resources),
                "maxClientCount": max(row.get("clientCount", 0) for row in resources),
                "maxWorkerCount": max(row.get("workerCount", 0) for row in resources),
            }
    publisher_path = os.path.join(run_dir, "publisher.json")
    if os.path.exists(publisher_path):
        data = load_result(publisher_path)
        stats = data.get("stats") or {}
        samples = data.get("publisherSamples") or []
        span = None
        capture_fps = None
        if len(samples) >= 2:
            span = (samples[-1]["epochMs"] - samples[0]["epochMs"]) / 1000.0
            if span > 0 and stats.get("capturedFrames"):
                capture_fps = float(stats["capturedFrames"]) / span
        result["publisher"] = {
            "ok": data.get("ok"),
            "error": data.get("error"),
            "exitCode": data.get("exitCode"),
            "profile": data.get("profile"),
            "codec": data.get("codec"),
            "viewerCount": data.get("viewerCount"),
            "maxViewers": data.get("maxViewers"),
            "capturedFrames": stats.get("capturedFrames"),
            "capturedFps": round(capture_fps, 2) if capture_fps else None,
            "uploadPackets": stats.get("uploadPackets"),
            "uploadDropped": stats.get("uploadDropped"),
            "uploadDroppedBudget": stats.get("uploadDroppedBudget"),
            "uploadDroppedAge": stats.get("uploadDroppedAge"),
            "uploadDroppedPacing": stats.get("uploadDroppedPacing"),
            "datagramsSent": stats.get("datagramsSent"),
            "peakOutboundBytes": stats.get("outboundPeakBytes"),
            "postStopOutboundBytes": (data.get("statsAfterStop") or {}).get("outboundBytes"),
            "postStopOutboundQueued": (data.get("statsAfterStop") or {}).get("outboundQueued"),
            "screenshot": data.get("screenshot"),
            "sampleSeconds": round(span, 1) if span else None,
        }
    for path in sorted(glob.glob(os.path.join(run_dir, "viewer*.json")),
                       key=lambda p: int("".join(filter(str.isdigit, os.path.basename(p))) or 0)):
        result["viewers"].append(summarise_viewer(path))
    capture_path = os.path.join(run_dir, "server-capture.json")
    if os.path.exists(capture_path):
        capture = load(capture_path)
        up = capture.get("uplink", {})
        down = capture.get("downlink", {})
        ratio = None
        if up.get("videoFragmentBytes"):
            ratio = down.get("videoFragmentBytes", 0) / up["videoFragmentBytes"]
        result["server"] = {
            "port": capture.get("port"),
            "uplink": {
                "datagrams": up.get("datagrams"),
                "udpPayloadBytes": up.get("udpPayloadBytes"),
                "videoFragmentBytes": up.get("videoFragmentBytes"),
                "zds1HeaderBytes": up.get("zds1HeaderBytes"),
                "zts1OuterBytes": up.get("zts1OuterBytes"),
                "udpIpOverheadBytes": up.get("udpIpOverheadBytes"),
                "registerDatagrams": up.get("registerDatagrams"),
            },
            "downlink": {
                "datagrams": down.get("datagrams"),
                "udpPayloadBytes": down.get("udpPayloadBytes"),
                "videoFragmentBytes": down.get("videoFragmentBytes"),
                "zds1HeaderBytes": down.get("zds1HeaderBytes"),
                "zts1OuterBytes": down.get("zts1OuterBytes"),
                "udpIpOverheadBytes": down.get("udpIpOverheadBytes"),
                "endpoints": len(down.get("endpoints") or {}),
                "endpointDetail": down.get("endpoints"),
            },
            "link": capture.get("link"),
            "videoEgressToIngressRatio": round(ratio, 3) if ratio is not None else None,
            "samples": len(capture.get("samples") or []),
            "peakRssMiB": max((s.get("rssMiB", 0) for s in capture.get("samples") or []), default=None),
            "peakHeapMiB": max((s.get("heapUsedMiB", 0) for s in capture.get("samples") or []), default=None),
            "peakThreads": max((s.get("threads", 0) for s in capture.get("samples") or []), default=None),
            "peakFds": max((s.get("fdCount", 0) for s in capture.get("samples") or []), default=None),
            "peakCpuPercent": max((s.get("cpuPercent", 0) for s in capture.get("samples") or []), default=None),
        }
    payload = json.dumps(result, indent=2, ensure_ascii=False)
    out = args.out or os.path.join(run_dir, "analysis.json")
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(payload)
    print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
