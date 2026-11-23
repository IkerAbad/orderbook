#!/usr/bin/env python3
"""Convert market-data-service trade Parquet into an orderbook replay CSV.

The service stores aggregate trades, not individual order events. A trade tells
us the executed price, the executed size and the aggressor side. To turn that
into the ``L``/``M``/``C`` trace the engine understands, this converter uses a
self-providing liquidity model:

* a limit order rests at the trade price on the maker side, with the trade
  size,
* a market order of the same size hits it on the aggressor side.

Every market order therefore matches exactly one limit order at the recorded
trade price. The book is flat between trades. This keeps the prices, sizes,
timing and aggressor mix of the real data without inventing resting depth or
cancellations that the public trade feed does not contain.

Prices are converted to integer ticks and sizes to integer steps. For
BTCUSDT the defaults are 0.01 quote units per tick and 0.00000001 BTC per
size unit, both exact decimals from the Parquet file.

Example:

    python tools/parquet_to_trace.py \
        --trades data/mds/trades/symbol=BTCUSDT/date=2026-11-22 \
        --out data/btcusdt-2026-11-22.csv
"""

from __future__ import annotations

import argparse
import hashlib
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.dataset as ds

EPOCH = datetime(1970, 1, 1, tzinfo=UTC)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trades", required=True, help="Parquet file or directory")
    parser.add_argument("--out", required=True, help="replay CSV to write")
    parser.add_argument("--symbol", help="keep only this symbol")
    parser.add_argument("--price-tick", default="0.01")
    parser.add_argument("--quantity-step", default="0.00000001")
    parser.add_argument("--limit", type=int, help="convert only the first N trades")
    parser.add_argument(
        "--absolute-time",
        action="store_true",
        help="keep epoch nanoseconds instead of starting at zero",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_trades(path: Path) -> ds.Dataset:
    if path.is_dir():
        files = sorted(path.rglob("*.parquet"))
        if not files:
            raise SystemExit(f"no Parquet files under {path}")
    else:
        files = [path]

    # An explicit file list keeps PyArrow from inferring Hive partitions from
    # symbol=.../date=... and colliding with the real columns of the service.
    return ds.dataset([str(file) for file in files], format="parquet")


def to_ns(value: datetime) -> int:
    delta = value - EPOCH
    return (
        (delta.days * 86_400 + delta.seconds) * 1_000_000_000
        + delta.microseconds * 1_000
    )


def to_units(value: Decimal, step: Decimal, field: str, row: int) -> int:
    scaled = value / step
    integral = scaled.to_integral_value()
    if scaled != integral:
        raise SystemExit(
            f"row {row}: {field} {value} is not a multiple of {step}"
        )
    return int(integral)


def main() -> int:
    args = parse_args()
    tick = Decimal(args.price_tick)
    step = Decimal(args.quantity_step)
    if tick <= 0 or step <= 0:
        raise SystemExit("--price-tick and --quantity-step must be positive")

    dataset = read_trades(Path(args.trades))
    schema_names = set(dataset.schema.names)
    time_column = "trade_time" if "trade_time" in schema_names else "event_time"
    required = {time_column, "price", "quantity", "is_buyer_maker"}
    missing = required - schema_names
    if missing:
        raise SystemExit(f"missing Parquet columns: {sorted(missing)}")

    columns = [time_column, "price", "quantity", "is_buyer_maker"]
    if "trade_id" in schema_names:
        columns.append("trade_id")
    if "symbol" in schema_names:
        columns.append("symbol")
    table = dataset.to_table(columns=columns)
    if args.symbol and "symbol" in table.column_names:
        table = table.filter(pc.equal(table["symbol"], args.symbol))
        table = table.drop_columns("symbol")
    if table.num_rows == 0:
        raise SystemExit("the trade dataset is empty")

    sort_keys = [(time_column, "ascending")]
    if "trade_id" in table.column_names:
        sort_keys.append(("trade_id", "ascending"))
    table = table.sort_by(sort_keys)
    if args.limit is not None:
        if args.limit < 1:
            raise SystemExit("--limit must be at least 1")
        table = table.slice(0, args.limit)

    times = table[time_column].to_pylist()
    prices = table["price"].to_pylist()
    quantities = table["quantity"].to_pylist()
    makers = table["is_buyer_maker"].to_pylist()

    first_ns = to_ns(times[0])
    previous_ns = first_ns
    next_id = 1
    events = 0
    volume = 0
    min_price = None
    max_price = None

    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="ascii", newline="\n") as handle:
        handle.write("ts_ns,op,side,price,quantity,id\n")
        for row, (when, price, quantity, maker) in enumerate(
            zip(times, prices, quantities, makers, strict=True)
        ):
            when_ns = to_ns(when)
            if when_ns < previous_ns:
                raise SystemExit(f"row {row}: timestamps are not monotonic")
            previous_ns = when_ns
            stamp = when_ns if args.absolute_time else when_ns - first_ns

            price_ticks = to_units(price, tick, "price", row)
            size = to_units(quantity, step, "quantity", row)
            if price_ticks <= 0 or size <= 0:
                raise SystemExit(f"row {row}: price and quantity must be positive")
            if price_ticks > 2**63 - 1:
                raise SystemExit(f"row {row}: price does not fit int64 ticks")
            if size > 2**64 - 1:
                raise SystemExit(f"row {row}: quantity does not fit uint64 steps")

            min_price = price_ticks if min_price is None else min(min_price, price_ticks)
            max_price = price_ticks if max_price is None else max(max_price, price_ticks)
            volume += size

            if maker:
                passive, aggressor = "B", "S"
            else:
                passive, aggressor = "S", "B"

            handle.write(
                f"{stamp},L,{passive},{price_ticks},{size},{next_id}\n"
            )
            handle.write(
                f"{stamp},M,{aggressor},{price_ticks},{size},0\n"
            )
            next_id += 1
            events += 2

    duration_s = (previous_ns - first_ns) / 1e9
    print(f"trades    {len(times)}")
    print(f"events    {events} (1 limit + 1 market per trade)")
    print(f"period    {times[0].isoformat()} .. {times[-1].isoformat()}")
    print(f"duration  {duration_s:.3f} s")
    print(f"price     {min_price} .. {max_price} ticks (tick {tick})")
    print(f"volume    {volume} steps ({Decimal(volume) * step} base units)")
    print(f"out       {output} ({output.stat().st_size} bytes)")
    print(f"sha256    {sha256(output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
