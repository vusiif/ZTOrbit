#!/usr/bin/env python3
"""RSS observations for stage-7 P3, separating stable and excluded windows.

Reads the independent resource samples (5 s cadence, full memory maps every
30 s) and the harness's compact per-second queue log, then reports per-PID
RSS/private memory, mapping counts, minute-level step candidates and business
queue bounds. Correlated counters cannot identify an allocation site. The
inherited window-bounded observation is separate from reproduction/location.
"""
from __future__ import annotations

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path

QUEUE_FIELDS = [
    "inboundQueued", "inboundBytes", "inboundPeakBytes", "inboundPeakPackets",
    "bufferedBytes", "peakBufferedBytes", "heldPackets", "heldBytes",
    "pendingPackets", "peakPendingPackets", "outboundQueued", "outboundBytes",
    "outboundPeakPackets", "outboundPeakBytes", "uploadDroppedQueue",
    "uploadDroppedBudget", "uploadDroppedDeadline", "uploadDroppedDependency",
    "uploadDroppedPacing", "uploadDroppedAge", "mediaReceivedecodedQueueSize",
    "mediaReceivedecodedQueuePeak", "mediaReceivedecodedQueueDrops",
    "guiPendingNotices", "guiPeakNotices", "missingPackets", "expiredFragments",
    "dropped", "duplicateDatagrams", "rejectedDatagrams",
]


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def num(value, default=0.0):
    try:
        if isinstance(value, bool):
            return float(value)
        return float(value)
    except (TypeError, ValueError):
        return default


def minute_key(epoch_ms, origin_ms):
    return int((epoch_ms - origin_ms) // 60000)


def minute_means(rows, role, field, origin_ms):
    by_minute = defaultdict(list)
    for row in rows:
        for entry in row.get("clients", []) + row.get("workers", []):
            if entry.get("role") == role and field in entry:
                by_minute[minute_key(row["epochMs"], origin_ms)].append(num(entry[field]))
    return {m: statistics.fmean(v) for m, v in sorted(by_minute.items()) if v}


def largest_jump(series):
    items = sorted(series.items())
    best = None
    for (m0, v0), (m1, v1) in zip(items, items[1:]):
        if m1 != m0 + 1:
            continue
        delta = v1 - v0
        if delta > 0 and (best is None or delta > best["deltaMiB"]):
            best = {"fromMinute": m0, "toMinute": m1, "fromMiB": round(v0, 2),
                    "toMiB": round(v1, 2), "deltaMiB": round(delta, 2)}
    return best


def queue_analysis(queue_dir):
    result = {}
    for path in sorted(Path(queue_dir).glob("queue-*.jsonl")):
        role = path.stem[len("queue-"):]
        maxima = {field: 0.0 for field in QUEUE_FIELDS}
        samples = 0
        first = last = None
        bad_lines = 0
        with path.open(encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    bad_lines += 1
                    continue
                pipeline = record.get("pipeline") or {}
                samples += 1
                epoch = record.get("epochMs")
                if first is None:
                    first = epoch
                last = epoch
                for field in QUEUE_FIELDS:
                    value = num(pipeline.get(field, 0))
                    if value > maxima[field]:
                        maxima[field] = value
        result[role] = {"samples": samples, "badLines": bad_lines,
                        "firstEpochMs": first, "lastEpochMs": last,
                        "maxima": maxima,
                        "nonzero": {k: v for k, v in maxima.items() if v},
                        "queueFile": path.name,
                        "queueBytes": path.stat().st_size}
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--stable-start-min", type=int, default=3)
    ap.add_argument("--stable-end-min", type=int, default=27)
    ap.add_argument("--expected-processes", type=int, default=18)
    args = ap.parse_args()
    run = Path(args.run)
    clients_dir = run / "clients" if (run / "clients" / "resources.json").exists() else run
    rows = load_json(clients_dir / "resources.json")
    rows = [r for r in rows if r.get("clientCount")]
    if not rows:
        raise SystemExit("no resource rows")
    origin = rows[0]["epochMs"]

    roles = sorted({e["role"] for r in rows for e in r.get("clients", [])
                    if e.get("role")})
    worker_roles = sorted({e["role"] for r in rows for e in r.get("workers", [])
                           if e.get("role")})

    complete = [r for r in rows
                if r.get("clientCount", 0) + r.get("workerCount", 0) == args.expected_processes]
    if not complete:
        raise SystemExit(f"no sample contains all {args.expected_processes} processes")
    stable = [r for r in complete
              if args.stable_start_min <= minute_key(r["epochMs"], origin) <= args.stable_end_min]
    if not stable:
        raise SystemExit("no sample inside the requested stable window")
    if args.stable_end_min - args.stable_start_min < 6:
        raise SystemExit("stable window must include the inherited six-minute observation")

    per_process = {}
    for role in roles + worker_roles:
        series = minute_means(rows, role, "rssMiB", origin)
        private = minute_means(rows, role, "privateMiB", origin)
        stable_series = {m: v for m, v in minute_means(complete, role, "rssMiB", origin).items()
                         if args.stable_start_min <= m <= args.stable_end_min}
        stable_private = {m: v for m, v in minute_means(complete, role, "privateMiB", origin).items()
                          if args.stable_start_min <= m <= args.stable_end_min}
        first = stable_series[min(stable_series)] if stable_series else None
        last = stable_series[max(stable_series)] if stable_series else None
        first_private = stable_private[min(stable_private)] if stable_private else None
        last_private = stable_private[max(stable_private)] if stable_private else None
        mapping_series = {}
        for r in complete:
            m = minute_key(r["epochMs"], origin)
            for e in r.get("clients", []) + r.get("workers", []):
                if e.get("role") == role and "mappings" in e:
                    mapping_series.setdefault(m, []).append(e["mappings"])
        mapping_means = {m: statistics.fmean(v) for m, v in mapping_series.items()}
        per_process[role] = {
            "minuteRssMiB": {str(k): round(v, 2) for k, v in series.items()},
            "minutePrivateMiB": {str(k): round(v, 2) for k, v in private.items()},
            "stableFirstRssMiB": round(first, 2) if first is not None else None,
            "stableLastRssMiB": round(last, 2) if last is not None else None,
            "stableDeltaRssMiB": round(last - first, 2) if first is not None and last is not None else None,
            "stableFirstPrivateMiB": round(first_private, 2) if first_private is not None else None,
            "stableLastPrivateMiB": round(last_private, 2) if last_private is not None else None,
            "stableDeltaPrivateMiB": round(last_private - first_private, 2)
            if first_private is not None and last_private is not None else None,
            "largestStableMinuteJump": largest_jump(stable_series),
            "mappingMinuteMean": {str(k): round(v, 1) for k, v in sorted(mapping_means.items())},
        }

    # Aggregate the nine clients (workers are reported separately). Each minute
    # is the mean of the per-row sums, not the sum of all rows: rows are not
    # guaranteed to be equally spaced, so summing rows would invent jumps.
    def aggregate(entries_key, roleset, source_rows):
        by_minute = defaultdict(list)
        for r in source_rows:
            m = minute_key(r["epochMs"], origin)
            total = 0.0
            seen = False
            for e in r.get(entries_key, []):
                if e.get("role") in roleset and "rssMiB" in e:
                    total += num(e["rssMiB"])
                    seen = True
            if seen:
                by_minute[m].append(total)
        return {m: statistics.fmean(v) for m, v in sorted(by_minute.items()) if v}

    client_totals = aggregate("clients", set(roles), rows)
    worker_totals = aggregate("workers", set(worker_roles), rows)
    stable_clients = {m: v for m, v in aggregate("clients", set(roles), complete).items()
                      if args.stable_start_min <= m <= args.stable_end_min}
    stable_workers = {m: v for m, v in aggregate("workers", set(worker_roles), complete).items()
                      if args.stable_start_min <= m <= args.stable_end_min}
    first, last = stable_clients[min(stable_clients)], stable_clients[max(stable_clients)]
    minutes = max(stable_clients) - min(stable_clients)
    last_six = {m: v for m, v in stable_clients.items() if m >= max(stable_clients) - 6}
    last_six_slope = ((last_six[max(last_six)] - last_six[min(last_six)])
                      / max(1, max(last_six) - min(last_six)))
    start_block = [stable_clients[m] for m in range(args.stable_start_min,
                                                   args.stable_start_min + 3)
                   if m in stable_clients]
    end_block = [stable_clients[m] for m in range(args.stable_end_min - 2,
                                                 args.stable_end_min + 1)
                 if m in stable_clients]
    three_minute = {
        "firstMeanMiB": round(statistics.fmean(start_block), 2) if start_block else None,
        "lastMeanMiB": round(statistics.fmean(end_block), 2) if end_block else None,
        "deltaMiB": round(statistics.fmean(end_block) - statistics.fmean(start_block), 2)
        if start_block and end_block else None,
        "minutes": {"start": [args.stable_start_min, args.stable_start_min + 2],
                    "end": [args.stable_end_min - 2, args.stable_end_min]},
    }

    queue = queue_analysis(clients_dir)
    # Both endpoints must belong to the stable window, with all processes.
    # Warm-up/exit and incomplete fleet samples are diagnostic only.
    jump = largest_jump(stable_clients)
    correlation = {"jump": jump, "workerDeltaAtJump": None, "mappingDeltaAtJump": None,
                   "interpretation": "time correlation only; not an allocation site"}
    if jump:
        m0, m1 = jump["fromMinute"], jump["toMinute"]
        if m0 in stable_workers and m1 in stable_workers:
            correlation["workerDeltaAtJump"] = round(stable_workers[m1] - stable_workers[m0], 2)
        mapping_by_minute = defaultdict(list)
        for r in stable:
            m = minute_key(r["epochMs"], origin)
            if "mappings" in r:
                mapping_by_minute[m].append(num(r["mappings"]))
        mapping_means = {m: statistics.fmean(v) for m, v in mapping_by_minute.items()}
        if m0 in mapping_means and m1 in mapping_means:
            correlation["mappingDeltaAtJump"] = int(mapping_means[m1] - mapping_means[m0])

    def aggregate_field(entries_key, roleset, field):
        by_minute = defaultdict(list)
        for r in stable:
            m = minute_key(r["epochMs"], origin)
            total = 0.0
            seen = False
            for e in r.get(entries_key, []):
                if e.get("role") in roleset and field in e:
                    total += num(e[field])
                    seen = True
            if seen:
                by_minute[m].append(total)
        return {m: statistics.fmean(v) for m, v in sorted(by_minute.items()) if v}

    client_threads = aggregate_field("clients", set(roles), "threads")
    client_handles = aggregate_field("clients", set(roles), "handles")
    thread_jump = largest_jump(client_threads)
    handle_jump = largest_jump(client_handles)
    thread_transient = {
        "threadsMinuteMean": {str(k): round(v, 1) for k, v in client_threads.items()},
        "handlesMinuteMean": {str(k): round(v, 1) for k, v in client_handles.items()},
        "largestThreadJump": thread_jump,
        "largestHandleJump": handle_jump,
        "rssDeltaAtThreadJump": None,
        "mappingDeltaAtThreadJump": None,
    }
    if thread_jump:
        m0, m1 = thread_jump["fromMinute"], thread_jump["toMinute"]
        if m0 in client_totals and m1 in client_totals:
            thread_transient["rssDeltaAtThreadJump"] = round(
                client_totals[m1] - client_totals[m0], 2)
        if thread_transient["mappingDeltaAtThreadJump"] is None:
            map_mean = {}
            for r in complete:
                m = minute_key(r["epochMs"], origin)
                if "mappings" in r:
                    map_mean.setdefault(m, []).append(num(r["mappings"]))
            map_mean = {m: statistics.fmean(v) for m, v in map_mean.items()}
            if m0 in map_mean and m1 in map_mean:
                thread_transient["mappingDeltaAtThreadJump"] = int(
                    map_mean[m1] - map_mean[m0])

    coverage = set(stable_clients) == set(range(args.stable_start_min, args.stable_end_min + 1))
    bounded = last_six_slope <= 1.0 if coverage else None
    verdict = ("bounded-but-unlocated" if bounded else "not-window-bounded-unlocated") if coverage else "insufficient-stable-coverage"
    warmup = {m: v for m, v in client_totals.items() if m < args.stable_start_min}
    exit_window = {m: v for m, v in client_totals.items() if m > args.stable_end_min}
    boundary_jumps = []
    for m in (args.stable_start_min - 1, args.stable_end_min):
        if m in client_totals and m + 1 in client_totals:
            boundary_jumps.append({"fromMinute": m, "toMinute": m + 1,
                                   "deltaMiB": round(client_totals[m + 1] - client_totals[m], 2)})
    result = {
        "schemaVersion": 1,
        "run": str(run),
        "expectedProcesses": args.expected_processes,
        "stableWindow": {"startMinute": args.stable_start_min,
                         "endMinute": args.stable_end_min},
        "sampleCount": len(rows),
        "completeSampleCount": len(complete),
        "stableSampleCount": len(stable),
        "processRoles": roles,
        "workerRoles": worker_roles,
        "clientTotalsRssMiB": {
            "minute": {str(k): round(v, 2) for k, v in client_totals.items()},
            "stableFirstMiB": round(first, 2), "stableLastMiB": round(last, 2),
            "stableDeltaMiB": round(last - first, 2),
            "stableSlopeMiBPerMin": round((last - first) / max(1, minutes), 3),
            "lastSixMinuteSlopeMiBPerMin": round(last_six_slope, 3),
            "firstLastThreeMinuteMeans": three_minute,
            "largestStableMinuteJump": jump,
            "largestWarmupMinuteJump": largest_jump(warmup),
            "largestExitMinuteJump": largest_jump(exit_window),
            "excludedBoundaryJumps": boundary_jumps,
        },
        "workerTotalsRssMiB": {
            "minute": {str(k): round(v, 2) for k, v in worker_totals.items()},
            "stableFirstMiB": round(stable_workers[min(stable_workers)], 2),
            "stableLastMiB": round(stable_workers[max(stable_workers)], 2),
            "stableDeltaMiB": round(stable_workers[max(stable_workers)]
                                    - stable_workers[min(stable_workers)], 2),
            "largestStableMinuteJump": largest_jump(stable_workers),
        },
        "perProcess": per_process,
        "threadHandleTransient": thread_transient,
        "queueBounds": queue,
        "correlation": correlation,
        "verdict": verdict,
        "windowObservation": {"completeMinuteCoverage": coverage,
                              "boundedByInheritedCriterion": bounded,
                              "criterion": "round-5: last six minutes <= 1 MiB/min; observation only"},
        "residualStep": {"observedAtLeast10MiB": bool(jump and jump["deltaMiB"] >= 10),
                         "independentReproduction": "not-assessed-by-single-run-analyzer",
                         "allocationSite": "unlocated"},
        "notes": [
            "RSS and private bytes are sampled every 5 s by PID/role; memory maps every 30 s.",
            "The per-second queue log is written by the test harness only when --queue-log is set; it does not retain sample history.",
            "Only complete fleet samples inside the stable window contribute to its metrics; minute buckets include both stated endpoints.",
            "The >=10 MiB reporting trigger and round-5 <=1 MiB/min observation are inherited, not acceptance thresholds; bounded is not a no-leak claim.",
            "Worker RSS, mapping counts and thread/handle changes are correlations, never allocation-site attribution. Queue bounds are whole-run maxima, not values at the step.",
        ],
    }
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"verdict": verdict, "clientDeltaMiB": result["clientTotalsRssMiB"]["stableDeltaMiB"],
                      "lastSixSlope": result["clientTotalsRssMiB"]["lastSixMinuteSlopeMiBPerMin"],
                      "largestJump": jump, "stableSamples": len(stable)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
