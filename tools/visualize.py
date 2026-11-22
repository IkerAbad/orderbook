#!/usr/bin/env python3
"""Render order book depth from a CSV produced by `orderbook depth`.

Usage:
    python tools/visualize.py --input data/depth.csv --png docs/depth.png --gif docs/depth.gif
"""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation, PillowWriter
from matplotlib.ticker import FuncFormatter

BID_COLOR = "#2e7d32"
ASK_COLOR = "#c62828"
INK = "#222222"
GRID = "#e2e2e2"

Frames = dict[int, dict[str, list[tuple[int, int]]]]


def load(path: Path) -> Frames:
    raw: dict[int, dict[str, list[tuple[int, int]]]] = defaultdict(
        lambda: {"B": [], "S": []}
    )
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            raw[int(row["frame"])][row["side"]].append(
                (int(row["price"]), int(row["quantity"]))
            )
    return {key: raw[key] for key in sorted(raw)}


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    index = int(fraction * (len(ordered) - 1))
    return ordered[index]


def limits(frames: Frames) -> tuple[int, int, int]:
    quantities = [q for frame in frames.values() for side in ("B", "S") for _, q in frame[side]]
    prices = [p for frame in frames.values() for side in ("B", "S") for p, _ in frame[side]]
    if not quantities or not prices:
        raise SystemExit("the CSV has no depth rows")
    return (
        max(1, percentile(quantities, 0.98)) * 11 // 10,
        percentile(prices, 0.02),
        percentile(prices, 0.98),
    )


def render(ax, data, max_quantity: int, low: int, high: int, title: str) -> None:
    ax.clear()

    bids = data["B"]
    asks = data["S"]

    if bids:
        ax.barh(
            [price for price, _ in bids],
            [-quantity for _, quantity in bids],
            height=0.9,
            color=BID_COLOR,
        )
    if asks:
        ax.barh(
            [price for price, _ in asks],
            [quantity for _, quantity in asks],
            height=0.9,
            color=ASK_COLOR,
        )

    ax.axvline(0.0, color=INK, linewidth=0.8)
    ax.set_xlim(-max_quantity * 1.15, max_quantity * 1.15)
    ax.set_ylim(low - 1, high + 1)
    ax.set_xlabel("quantity  (bids left, asks right)")
    ax.set_ylabel("price (ticks)")
    ax.set_title(title)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{abs(int(value))}"))
    ax.grid(axis="y", color=GRID, linewidth=0.5)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)


def write_png(frames: Frames, path: Path, max_quantity: int, low: int, high: int) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.5), dpi=120)
    key = list(frames)[-1]
    data = frames[key]
    local_quantities = [q for side in ("B", "S") for _, q in data[side]]
    local_prices = [p for side in ("B", "S") for p, _ in data[side]]
    render(
        ax,
        data,
        max(1, percentile(local_quantities, 0.98)) * 11 // 10,
        min(local_prices),
        max(local_prices),
        f"Order book depth - frame {key + 1}/{len(frames)}",
    )
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path)
    plt.close(fig)


def write_gif(frames: Frames, path: Path, max_quantity: int, low: int, high: int) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.5), dpi=100)
    keys = list(frames)

    def update(index: int) -> None:
        key = keys[index]
        render(
            ax,
            frames[key],
            max_quantity,
            low,
            high,
            f"Order book depth - frame {key + 1}/{len(frames)}",
        )

    animation = FuncAnimation(fig, update, frames=len(keys), interval=80)
    path.parent.mkdir(parents=True, exist_ok=True)
    animation.save(path, writer=PillowWriter(fps=12))
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--png", type=Path)
    parser.add_argument("--gif", type=Path)
    args = parser.parse_args()

    frames = load(args.input)
    if not frames:
        raise SystemExit("the CSV has no frames")

    max_quantity, low, high = limits(frames)

    if args.png:
        write_png(frames, args.png, max_quantity, low, high)
        print(f"wrote {args.png}")
    if args.gif:
        write_gif(frames, args.gif, max_quantity, low, high)
        print(f"wrote {args.gif}")


if __name__ == "__main__":
    main()
