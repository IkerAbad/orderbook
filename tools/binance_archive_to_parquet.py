#!/usr/bin/env python3
"""Normalize a Binance daily aggTrades archive into the service schema.

``market-data-service`` records live ``BTCUSDT`` trades as Parquet. A full
historical day is published by Binance as a daily ZIP. This script writes that
ZIP in the same ``trades/symbol=.../date=...`` layout and with the same Arrow
types as the service, so ``parquet_to_trace.py`` can read either source without
special cases.

The archive has one timestamp per aggregate trade (``transact_time``). The
service stores ``event_time`` and ``trade_time`` separately because the live
stream carries both. For the archive both columns are set to ``transact_time``
and ``ingested_at`` is set to the same instant: no local receive time exists
for a historical file.

Example:

    python tools/binance_archive_to_parquet.py \
        --date 2026-11-22 --out data/mds
"""

from __future__ import annotations

import argparse
import hashlib
import urllib.request
import zipfile
from datetime import date as date_type
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.csv as pcsv
import pyarrow.parquet as pq

DECIMAL = pa.decimal128(38, 12)
TS = pa.timestamp("us", tz="UTC")

TRADE_SCHEMA = pa.schema(
    [
        pa.field("symbol", pa.string(), nullable=False),
        pa.field("trade_id", pa.uint64(), nullable=False),
        pa.field("price", DECIMAL, nullable=False),
        pa.field("quantity", DECIMAL, nullable=False),
        pa.field("quote_quantity", DECIMAL, nullable=False),
        pa.field("event_time", TS, nullable=False),
        pa.field("trade_time", TS, nullable=False),
        pa.field("first_trade_id", pa.uint64(), nullable=False),
        pa.field("last_trade_id", pa.uint64(), nullable=False),
        pa.field("is_buyer_maker", pa.bool_(), nullable=False),
        pa.field("ingested_at", TS, nullable=False),
    ],
    metadata={
        "schema": "market-data-service/trades/v1",
        "price_scale": "12",
        "timezone": "UTC",
    },
)

ARCHIVE_COLUMNS = [
    "agg_trade_id",
    "price",
    "quantity",
    "first_trade_id",
    "last_trade_id",
    "transact_time",
    "is_buyer_maker",
    "is_best_match",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--symbol", default="BTCUSDT")
    parser.add_argument("--date", required=True, help="UTC date, YYYY-MM-DD")
    parser.add_argument("--out", required=True, help="dataset root, e.g. data/mds")
    parser.add_argument("--zip", help="use an already downloaded archive")
    parser.add_argument(
        "--cache-dir",
        default="data/binance-archive",
        help="where to keep the downloaded ZIP",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def archive_url(symbol: str, day: str) -> str:
    return (
        "https://data.binance.vision/data/spot/daily/aggTrades/"
        f"{symbol}/{symbol}-aggTrades-{day}.zip"
    )


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".zip.tmp")
    print(f"download {url}")
    urllib.request.urlretrieve(url, temporary)
    temporary.replace(destination)


def main() -> int:
    args = parse_args()
    day = date_type.fromisoformat(args.date)
    symbol = args.symbol.upper()

    if args.zip:
        zip_path = Path(args.zip)
    else:
        zip_path = Path(args.cache_dir) / f"{symbol}-aggTrades-{day.isoformat()}.zip"
        if not zip_path.exists():
            download(archive_url(symbol, day.isoformat()), zip_path)

    archive_hash = sha256(zip_path)
    print(f"archive {zip_path} sha256 {archive_hash}")

    with zipfile.ZipFile(zip_path) as archive:
        members = [name for name in archive.namelist() if name.endswith(".csv")]
        if len(members) != 1:
            raise SystemExit(f"expected one CSV in {zip_path}, found {members}")
        with archive.open(members[0]) as handle:
            table = pcsv.read_csv(
                handle,
                read_options=pcsv.ReadOptions(
                    column_names=ARCHIVE_COLUMNS, skip_rows=0
                ),
                convert_options=pcsv.ConvertOptions(
                    column_types={"price": "string", "quantity": "string"}
                ),
            )

    rows = table.num_rows
    if rows == 0:
        raise SystemExit("the archive has no rows")

    price = pc.cast(table["price"], pa.decimal128(18, 8))
    quantity = pc.cast(table["quantity"], pa.decimal128(18, 8))
    quote = pc.cast(pc.multiply(price, quantity), DECIMAL)
    event_time = pc.cast(table["transact_time"], TS)

    arrays = [
        pa.array([symbol] * rows, type=pa.string()),
        pc.cast(table["agg_trade_id"], pa.uint64()),
        pc.cast(price, DECIMAL),
        pc.cast(quantity, DECIMAL),
        quote,
        event_time,
        event_time,
        pc.cast(table["first_trade_id"], pa.uint64()),
        pc.cast(table["last_trade_id"], pa.uint64()),
        table["is_buyer_maker"],
        event_time,
    ]
    output = pa.Table.from_arrays(arrays, schema=TRADE_SCHEMA)

    directory = Path(args.out) / "trades" / f"symbol={symbol}" / f"date={day.isoformat()}"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"part-{day.strftime('%Y%m%d')}T000000000000Z-{archive_hash[:8]}.parquet"
    pq.write_table(output, path, compression="zstd", use_dictionary=True)

    first = output["trade_time"][0].as_py()
    last = output["trade_time"][-1].as_py()
    print(
        f"wrote {rows} trades to {path} "
        f"({first.isoformat()} .. {last.isoformat()})"
    )
    print(f"parquet sha256 {sha256(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
