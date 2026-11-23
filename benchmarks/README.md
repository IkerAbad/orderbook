# Real trace: BTCUSDT, 2026-11-22

This directory holds one full UTC day of real BTCUSDT aggregate trades,
converted to the replay CSV that `orderbook` reads. It is the input for the
real-data benchmark in the top-level README.

## Provenance

| Item | Value |
|---|---|
| Source | `https://data.binance.vision/data/spot/daily/aggTrades/BTCUSDT/BTCUSDT-aggTrades-2026-11-22.zip` |
| Source SHA256 | `5b669e0c375e56eea111b044e2c55bfdb2901832f8db61593b9fbed2580a4f68` |
| Trades | 1,003,991 aggregate trades |
| Period | 2026-11-22T00:00:00.695991Z .. 2026-11-22T23:59:59.266267Z |
| Price range | 58,768.51 .. 61,669.99 USDT |
| MDS-format Parquet SHA256 | `3903205af34d826cb14f641dc70ccc1084e2d76f164ad8252fb1de797de8fdc9` |
| Replay CSV SHA256 | `fe888ae4d141180ed55c0fc7984a7da38c0697d711714911d202102cb460efbc` |
| Compressed CSV SHA256 | `8f5e3f0448e84e240b06e1472b122c3c46e642b6c7c8d038ea4a7ba5cfcb00ef` |

The daily archive was used because the local `market-data-service` records live
data and was not running for the whole day. `binance_archive_to_parquet.py`
writes the archive with the same Arrow schema and
`trades/symbol=.../date=...` layout as the service. The converter was also run
against a live service capture, 27 Parquet files from 2026-11-23, and the
replay agreed across all three books.

## Files

- `btcusdt-2026-11-22.csv.zst`: the replay CSV, zstd-compressed.
- `btcusdt-2026-11-22.sha256`: hashes of the compressed and decompressed files.
- `btcusdt-2026-11-22.json`: output of `trace_report.py` for the real trace.
- `synthetic-mixed-seed42.json`: output of `trace_report.py` for the synthetic
  reference trace.

## Regenerate

The archive is cached under `data/binance-archive/`, which is gitignored.

```bash
python tools/binance_archive_to_parquet.py \
  --date 2026-11-22 --out data/mds

python tools/parquet_to_trace.py \
  --trades data/mds/trades/symbol=BTCUSDT/date=2026-11-22 \
  --symbol BTCUSDT --price-tick 0.01 --quantity-step 0.00000001 \
  --out data/btcusdt-2026-11-22.csv

python tools/trace_report.py \
  --trace data/btcusdt-2026-11-22.csv \
  --label "BTCUSDT 2026-11-22, 1003991 aggTrades" \
  --price-tick 0.01 --quantity-step 0.00000001 \
  --json-out benchmarks/btcusdt-2026-11-22.json
```

## Verify and run

```bash
zstd -d benchmarks/btcusdt-2026-11-22.csv.zst -o data/btcusdt-2026-11-22.csv
sha256sum data/btcusdt-2026-11-22.csv

./build/orderbook replay data/btcusdt-2026-11-22.csv --impl all
./build/orderbook bench --trace data/btcusdt-2026-11-22.csv \
  --runs 3 --repeat 10 --impl all
```

## Conversion model

The service stores aggregate trades, not individual order events. For each
trade the converter writes one limit order and one market order:

- the limit order rests at the trade price on the maker side, with the trade
  size;
- the market order of the same size hits it on the aggressor side.

This preserves the real price, size, timestamp and aggressor side of every
trade. It also means the book is flat between trades: the trace has no resting
depth and no cancellations, because the public aggregate trade feed does not
publish them. The synthetic workloads remain the stress test for a deep book
and for cancels.

Prices are integer ticks at 0.01 USDT and sizes are integer steps of
0.00000001 BTC. Both scales are exact for BTCUSDT and are recorded in the
converter output.
