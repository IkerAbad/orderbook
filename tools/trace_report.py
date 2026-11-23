#!/usr/bin/env python3
"""Measure a replay CSV: event mix, sizes, bursts and cancellations.

Every number published in the README comes from this script. It reads the same
file the engine reads, so the report and the benchmark cannot drift apart.

Example:

    python tools/trace_report.py \
        --trace data/btcusdt-2026-11-22.csv \
        --label "BTCUSDT 2026-11-22" \
        --price-tick 0.01 --quantity-step 0.00000001 \
        --json-out data/btcusdt-2026-11-22.json \
        --md-out data/btcusdt-2026-11-22.md
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import deque
from decimal import Decimal
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--price-tick", default="1")
    parser.add_argument("--quantity-step", default="1")
    parser.add_argument("--json-out")
    parser.add_argument("--md-out")
    return parser.parse_args()


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    rank = max(1, math.ceil(fraction * len(values)))
    return values[rank - 1]


def summarize(values: list[int]) -> dict[str, float | int]:
    if not values:
        return {
            "count": 0,
            "mean": 0.0,
            "p50": 0,
            "p90": 0,
            "p99": 0,
            "max": 0,
            "p99_over_p50": 0.0,
        }
    values.sort()
    p50 = percentile(values, 0.50)
    p99 = percentile(values, 0.99)
    return {
        "count": len(values),
        "mean": sum(values) / len(values),
        "p50": p50,
        "p90": percentile(values, 0.90),
        "p99": p99,
        "max": values[-1],
        "p99_over_p50": (p99 / p50) if p50 else 0.0,
    }


def main() -> int:
    args = parse_args()
    limits: list[int] = []
    markets: list[int] = []
    prices: list[int] = []
    timestamps: list[int] = []
    counts = {"L": 0, "M": 0, "C": 0}
    first_ns = None
    previous_ns = None
    max_gap_ns = 0

    windows = {
        "10ms": deque(),
        "100ms": deque(),
        "1s": deque(),
    }
    maxima = {"10ms": 0, "100ms": 0, "1s": 0}
    trade_window = deque()
    max_trades_1s = 0

    with Path(args.trace).open("r", encoding="ascii", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader)
        if header != ["ts_ns", "op", "side", "price", "quantity", "id"]:
            raise SystemExit(f"unexpected header {header}")
        for line, row in enumerate(reader, start=2):
            if len(row) != 6:
                raise SystemExit(f"line {line}: expected 6 fields")
            stamp = int(row[0])
            op = row[1]
            price = int(row[3])
            quantity = int(row[4])
            if op not in counts:
                raise SystemExit(f"line {line}: unknown op {op!r}")

            counts[op] += 1
            if first_ns is None:
                first_ns = stamp
            if previous_ns is not None:
                if stamp < previous_ns:
                    raise SystemExit(f"line {line}: timestamps go backwards")
                max_gap_ns = max(max_gap_ns, stamp - previous_ns)
            previous_ns = stamp
            timestamps.append(stamp)

            for name, window in windows.items():
                cutoff = stamp - {
                    "10ms": 10_000_000,
                    "100ms": 100_000_000,
                    "1s": 1_000_000_000,
                }[name]
                while window and window[0] <= cutoff:
                    window.popleft()
                window.append(stamp)
                maxima[name] = max(maxima[name], len(window))

            if op == "C":
                continue
            if op == "L":
                prices.append(price)
                limits.append(quantity)
            else:
                markets.append(quantity)

            if op == "M":
                while trade_window and trade_window[0] <= stamp - 1_000_000_000:
                    trade_window.popleft()
                trade_window.append(stamp)
                max_trades_1s = max(max_trades_1s, len(trade_window))

    if not timestamps:
        raise SystemExit("the trace is empty")

    duration_s = (timestamps[-1] - timestamps[0]) / 1e9
    events = len(timestamps)
    cancels = counts["C"]
    limits_and_cancels = counts["L"] + cancels
    tick = Decimal(args.price_tick)
    step = Decimal(args.quantity_step)

    report = {
        "label": args.label,
        "events": events,
        "limits": counts["L"],
        "cancels": cancels,
        "markets": counts["M"],
        "cancel_share": cancels / events if events else 0.0,
        "cancel_share_of_limit": (
            cancels / limits_and_cancels if limits_and_cancels else 0.0
        ),
        "duration_s": duration_s,
        "events_per_second": events / duration_s if duration_s else 0.0,
        "trades_per_second": counts["M"] / duration_s if duration_s else 0.0,
        "max_events_10ms": maxima["10ms"],
        "max_events_100ms": maxima["100ms"],
        "max_events_1s": maxima["1s"],
        "max_trades_1s": max_trades_1s,
        "max_gap_ms": max_gap_ns / 1e6,
        "price_min_ticks": min(prices) if prices else 0,
        "price_max_ticks": max(prices) if prices else 0,
        "price_min": str(Decimal(min(prices)) * tick) if prices else "0",
        "price_max": str(Decimal(max(prices)) * tick) if prices else "0",
        "size_limit": summarize(limits),
        "size_market": summarize(markets),
        "size_all": summarize(limits + markets),
        "price_tick": args.price_tick,
        "quantity_step": args.quantity_step,
    }

    # Human units for the table. The JSON keeps raw trace units.
    def base(value: int) -> str:
        return str(Decimal(value) * step)

    def table_size(row: dict[str, float | int]) -> str:
        return (
            f"{base(int(row['p50']))} / {base(int(row['p90']))} / "
            f"{base(int(row['p99']))} / {base(int(row['max']))}"
        )

    markdown = f"""### {args.label}

| Metric | Value |
|---|---|
| Events | {report['events']} |
| Limit / cancel / market | {report['limits']} / {report['cancels']} / {report['markets']} |
| Cancel share | {report['cancel_share']:.2%} of events, {report['cancel_share_of_limit']:.2%} of limit + cancel |
| Duration | {report['duration_s']:.3f} s |
| Events per second | {report['events_per_second']:.2f} |
| Trades per second | {report['trades_per_second']:.2f} |
| Max events, 10 ms / 100 ms / 1 s | {report['max_events_10ms']} / {report['max_events_100ms']} / {report['max_events_1s']} |
| Max trades, 1 s | {report['max_trades_1s']} |
| Max inter-arrival gap | {report['max_gap_ms']:.3f} ms |
| Price range | {report['price_min']} .. {report['price_max']} |
| Limit size p50 / p90 / p99 / max | {table_size(report['size_limit'])} |
| Market size p50 / p90 / p99 / max | {table_size(report['size_market'])} |
| All size p50 / p90 / p99 / max | {table_size(report['size_all'])} |
| p99 / p50 | {report['size_all']['p99_over_p50']:.2f} |
"""
    print(markdown)

    if args.json_out:
        path = Path(args.json_out)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.md_out:
        path = Path(args.md_out)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(markdown, encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
