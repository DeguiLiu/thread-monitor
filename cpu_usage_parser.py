#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cpu_usage_parser.py - Parse and visualize thread-level CPU usage data.

Supports binary format v2 (magic "CMON") with footer thread map updates.
Also supports legacy v1 format (no magic) for backward compatibility.
Features: matplotlib plots, CSV export, percentile statistics.
"""

from __future__ import annotations

import argparse
import struct
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Binary format constants
# ---------------------------------------------------------------------------
MAGIC = 0x4E4F4D43        # "CMON"
FOOTER_MAGIC = 0x444E4543  # "CEND"
RECORD_FORMAT = "<IIHHBBbb"  # 16 bytes
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
# Legacy v1 record: 22 bytes
LEGACY_RECORD_FORMAT = "<HHIIIIBB"
LEGACY_RECORD_SIZE = struct.calcsize(LEGACY_RECORD_FORMAT)

assert RECORD_SIZE == 16


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------
class FileHeader:
    __slots__ = ("version", "process_name", "num_cpus", "ticks_per_sec",
                 "thread_map")

    def __init__(self) -> None:
        self.version: int = 0
        self.process_name: str = ""
        self.num_cpus: int = 0
        self.ticks_per_sec: int = 0
        self.thread_map: Dict[int, str] = {}


class CpuRecord:
    __slots__ = ("timestamp", "thread_id", "user_pct", "kernel_pct",
                 "state", "processor", "priority", "nice")

    def __init__(self, timestamp: int, thread_id: int, user_pct: int,
                 kernel_pct: int, state: int, processor: int,
                 priority: int, nice: int) -> None:
        self.timestamp = timestamp
        self.thread_id = thread_id
        self.user_pct = user_pct
        self.kernel_pct = kernel_pct
        self.state = state
        self.processor = processor
        self.priority = priority
        self.nice = nice

    @property
    def user_percent(self) -> float:
        return self.user_pct / 100.0

    @property
    def kernel_percent(self) -> float:
        return self.kernel_pct / 100.0

    @property
    def total_percent(self) -> float:
        return (self.user_pct + self.kernel_pct) / 100.0


# ---------------------------------------------------------------------------
# Binary file parser
# ---------------------------------------------------------------------------
def _read_u32(f) -> int:
    data = f.read(4)
    if len(data) < 4:
        raise ValueError("Unexpected EOF reading uint32")
    return struct.unpack("<I", data)[0]


def _read_string(f, length: int) -> str:
    data = f.read(length)
    if len(data) < length:
        raise ValueError("Unexpected EOF reading string")
    return data.decode("utf-8", errors="replace")


def _read_thread_map(f) -> Dict[int, str]:
    count = _read_u32(f)
    tmap: Dict[int, str] = {}
    for _ in range(count):
        tid = _read_u32(f)
        name_len = _read_u32(f)
        name = _read_string(f, name_len)
        tmap[tid] = name
    return tmap


def read_binary(filename: str) -> Tuple[FileHeader, List[CpuRecord]]:
    """Read and parse binary file (v1 or v2)."""
    header = FileHeader()
    records: List[CpuRecord] = []

    with open(filename, "rb") as f:
        magic = _read_u32(f)
        if magic == MAGIC:
            header.version = _read_u32(f)
            _parse_v2_header(f, header)
            _read_v2_records(f, header, records)
        else:
            # Legacy v1
            header.version = 1
            f.seek(0)
            _parse_v1_header(f, header)
            _read_v1_records(f, records)

    return header, records


def _parse_v2_header(f, header: FileHeader) -> None:
    header_size = _read_u32(f)
    start = f.tell()
    pname_len = _read_u32(f)
    header.process_name = _read_string(f, pname_len)
    header.num_cpus = _read_u32(f)
    header.ticks_per_sec = _read_u32(f)
    header.thread_map = _read_thread_map(f)
    f.seek(start + header_size)


def _parse_v1_header(f, header: FileHeader) -> None:
    header_size = _read_u32(f)
    start = f.tell()
    pname_len = _read_u32(f)
    header.process_name = _read_string(f, pname_len)
    header.thread_map = _read_thread_map(f)
    f.seek(start + header_size)


def _read_v2_records(f, header: FileHeader,
                     records: List[CpuRecord]) -> None:
    rec_st = struct.Struct(RECORD_FORMAT)
    while True:
        data = f.read(RECORD_SIZE)
        if not data or len(data) < RECORD_SIZE:
            break
        # Check footer magic
        if struct.unpack_from("<I", data, 0)[0] == FOOTER_MAGIC:
            f.seek(f.tell() - RECORD_SIZE)
            _read_u32(f)  # skip magic
            new_threads = _read_thread_map(f)
            header.thread_map.update(new_threads)
            break
        vals = rec_st.unpack(data)
        records.append(CpuRecord(*vals))


def _read_v1_records(f, records: List[CpuRecord]) -> None:
    """Read legacy 22-byte records."""
    rec_st = struct.Struct(LEGACY_RECORD_FORMAT)
    while True:
        data = f.read(LEGACY_RECORD_SIZE)
        if not data or len(data) < LEGACY_RECORD_SIZE:
            break
        vals = rec_st.unpack(data)
        # v1: user_pct, kernel_pct, user_ticks, kernel_ticks,
        #     timestamp, thread_id, status, flags
        records.append(CpuRecord(
            timestamp=vals[4], thread_id=vals[5],
            user_pct=vals[0], kernel_pct=vals[1],
            state=vals[6], processor=0,
            priority=vals[7], nice=0,
        ))


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------
def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (k - lo) * (s[hi] - s[lo])


def compute_thread_stats(records: List[CpuRecord],
                         thread_map: Dict[int, str]) -> List[dict]:
    by_thread: Dict[int, List[CpuRecord]] = {}
    for r in records:
        by_thread.setdefault(r.thread_id, []).append(r)

    stats = []
    for tid, recs in by_thread.items():
        totals = [r.total_percent for r in recs]
        users = [r.user_percent for r in recs]
        kernels = [r.kernel_percent for r in recs]
        n = len(totals)
        stats.append({
            "tid": tid,
            "name": thread_map.get(tid, f"tid_{tid}"),
            "samples": n,
            "avg_total": sum(totals) / n,
            "max_total": max(totals),
            "p50_total": percentile(totals, 50),
            "p95_total": percentile(totals, 95),
            "p99_total": percentile(totals, 99),
            "avg_user": sum(users) / n,
            "avg_kernel": sum(kernels) / n,
        })

    stats.sort(key=lambda s: s["avg_total"], reverse=True)
    return stats


def print_summary(header: FileHeader, records: List[CpuRecord]) -> None:
    stats = compute_thread_stats(records, header.thread_map)
    print(f"Process: {header.process_name}  "
          f"CPUs: {header.num_cpus}  "
          f"Samples: {len(records)}  "
          f"Threads: {len(stats)}")

    if not stats:
        print("No data.")
        return

    timestamps = [r.timestamp for r in records]
    t_min = datetime.fromtimestamp(min(timestamps))
    t_max = datetime.fromtimestamp(max(timestamps))
    print(f"Time range: {t_min} ~ {t_max}\n")

    fmt = "{:<24s} {:>7s} {:>7s} {:>7s} {:>7s} {:>7s} {:>7s} {:>7s}"
    print(fmt.format("Thread", "Avg%", "Max%", "P50%", "P95%", "P99%",
                      "User%", "Kern%"))
    print("-" * 88)
    for s in stats:
        print(fmt.format(
            s["name"][:24],
            f"{s['avg_total']:.1f}", f"{s['max_total']:.1f}",
            f"{s['p50_total']:.1f}", f"{s['p95_total']:.1f}",
            f"{s['p99_total']:.1f}",
            f"{s['avg_user']:.1f}", f"{s['avg_kernel']:.1f}",
        ))
    print("-" * 88)

    by_ts: Dict[int, float] = {}
    for r in records:
        by_ts[r.timestamp] = by_ts.get(r.timestamp, 0.0) + r.total_percent
    proc_totals = list(by_ts.values())
    if proc_totals:
        print(f"Process total: avg={sum(proc_totals)/len(proc_totals):.1f}%  "
              f"max={max(proc_totals):.1f}%  "
              f"p50={percentile(proc_totals, 50):.1f}%  "
              f"p95={percentile(proc_totals, 95):.1f}%")


# ---------------------------------------------------------------------------
# CSV export
# ---------------------------------------------------------------------------
def export_csv(header: FileHeader, records: List[CpuRecord],
               output: str) -> None:
    with open(output, "w") as f:
        f.write("timestamp,datetime,thread_id,thread_name,"
                "user_pct,kernel_pct,total_pct,state,cpu,priority,nice\n")
        for r in records:
            name = header.thread_map.get(r.thread_id, f"tid_{r.thread_id}")
            dt = datetime.fromtimestamp(r.timestamp).strftime(
                "%Y-%m-%d %H:%M:%S")
            st = chr(r.state) if 32 < r.state < 127 else "?"
            f.write(f"{r.timestamp},{dt},{r.thread_id},{name},"
                    f"{r.user_percent:.2f},{r.kernel_percent:.2f},"
                    f"{r.total_percent:.2f},{st},"
                    f"{r.processor},{r.priority},{r.nice}\n")
    print(f"CSV exported: {output} ({len(records)} records)")


# ---------------------------------------------------------------------------
# Plotting
# ---------------------------------------------------------------------------
def plot_cpu_usage(header: FileHeader, records: List[CpuRecord],
                   output_file: str = "cpu_usage.png",
                   top_n: int = 10, separate: bool = False,
                   filter_thread: Optional[str] = None,
                   show_summary: bool = True) -> None:
    try:
        import matplotlib.pyplot as plt
        import matplotlib.font_manager as fm
    except ImportError:
        print("matplotlib not installed. Install: pip install matplotlib")
        return

    if not records:
        print("No data to plot.")
        return

    tmap = header.thread_map

    # Build per-thread time series
    by_tid: Dict[int, Tuple[List[datetime], List[float], List[float]]] = {}
    for r in records:
        name = tmap.get(r.thread_id, f"tid_{r.thread_id}")
        if filter_thread and filter_thread.lower() not in name.lower():
            continue
        if r.thread_id not in by_tid:
            by_tid[r.thread_id] = ([], [], [])
        ts, us, ks = by_tid[r.thread_id]
        ts.append(datetime.fromtimestamp(r.timestamp))
        us.append(r.user_percent)
        ks.append(r.kernel_percent)

    if not by_tid:
        print("No matching threads.")
        return

    # Rank by avg total
    ranked = sorted(by_tid.keys(), key=lambda tid: (
        sum(u + k for u, k in zip(by_tid[tid][1], by_tid[tid][2]))
        / max(len(by_tid[tid][0]), 1)
    ), reverse=True)
    top_tids = ranked[:top_n]

    # Process total
    proc_by_ts: Dict[int, float] = {}
    for r in records:
        proc_by_ts[r.timestamp] = (
            proc_by_ts.get(r.timestamp, 0.0) + r.total_percent)
    proc_times = sorted(proc_by_ts.keys())
    proc_dts = [datetime.fromtimestamp(t) for t in proc_times]
    proc_vals = [proc_by_ts[t] for t in proc_times]

    fig_h = 10 if show_summary else 7
    fig, ax = plt.subplots(figsize=(14, fig_h))

    ax.plot(proc_dts, proc_vals, label="Process Total",
            color="black", linewidth=2, alpha=0.7)

    colors = list(plt.cm.tab10.colors) + list(plt.cm.Set2.colors)
    for i, tid in enumerate(top_tids):
        ts_list, users, kernels = by_tid[tid]
        name = tmap.get(tid, f"tid_{tid}")
        c = colors[i % len(colors)]
        if separate:
            ax.plot(ts_list, users, label=f"{name} (user)",
                    color=c, linestyle="-", alpha=0.8)
            ax.plot(ts_list, kernels, label=f"{name} (kern)",
                    color=c, linestyle="--", alpha=0.6)
        else:
            totals = [u + k for u, k in zip(users, kernels)]
            ax.plot(ts_list, totals, label=name, color=c, alpha=0.8)

    ax.set_xlabel("Time")
    ax.set_ylabel("CPU Usage (%)")
    ax.set_title(f"CPU Usage: {header.process_name}")
    ax.legend(loc="upper left", bbox_to_anchor=(1.01, 1), fontsize=8)
    ax.grid(True, alpha=0.3)
    fig.autofmt_xdate()

    if show_summary:
        stats = compute_thread_stats(records, tmap)
        lines = [f"Process: {header.process_name}  "
                 f"CPUs: {header.num_cpus}  Threads: {len(stats)}"]
        lines.append(f"{'Thread':<20s} {'Avg%':>6s} {'Max%':>6s} "
                     f"{'P50%':>6s} {'P95%':>6s}")
        lines.append("-" * 48)
        for s in stats[:top_n]:
            lines.append(f"{s['name'][:20]:<20s} {s['avg_total']:>6.1f} "
                         f"{s['max_total']:>6.1f} {s['p50_total']:>6.1f} "
                         f"{s['p95_total']:>6.1f}")
        mono = fm.FontProperties(family="monospace", size=8)
        fig.subplots_adjust(bottom=0.25, right=0.82)
        fig.text(0.02, 0.01, "\n".join(lines), fontproperties=mono,
                 verticalalignment="bottom",
                 bbox=dict(facecolor="white", alpha=0.8, edgecolor="gray"))
    else:
        fig.tight_layout(rect=[0, 0.02, 0.82, 0.98])

    fig.savefig(output_file, dpi=150, bbox_inches="tight")
    print(f"Plot saved: {output_file}")
    try:
        plt.show()
    except KeyboardInterrupt:
        plt.close()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main() -> None:
    p = argparse.ArgumentParser(
        description="Parse and visualize thread CPU usage data.")
    p.add_argument("filename", help="Binary data file (.bin)")
    p.add_argument("--csv", metavar="FILE", help="Export to CSV")
    p.add_argument("--plot", metavar="FILE", nargs="?",
                   const="cpu_usage.png", help="Generate plot image")
    p.add_argument("--top", type=int, default=10,
                   help="Top N threads (default: 10)")
    p.add_argument("--filter-thread", type=str,
                   help="Filter threads by name substring")
    p.add_argument("--separate-cpu", action="store_true",
                   help="Plot user/kernel separately")
    p.add_argument("--hide-summary", action="store_true",
                   help="Hide summary in plot")
    p.add_argument("--no-plot", action="store_true",
                   help="Summary only, no plot")

    args = p.parse_args()

    if not Path(args.filename).exists():
        print(f"Error: file not found: {args.filename}")
        sys.exit(1)

    header, records = read_binary(args.filename)
    if not records:
        print("No records found.")
        sys.exit(1)

    print_summary(header, records)

    if args.csv:
        export_csv(header, records, args.csv)

    if args.no_plot:
        return

    plot_file = args.plot or "cpu_usage.png"
    plot_cpu_usage(header, records, output_file=plot_file, top_n=args.top,
                   separate=args.separate_cpu,
                   filter_thread=args.filter_thread,
                   show_summary=not args.hide_summary)


if __name__ == "__main__":
    main()
