# Benchmarks

How kvlog's throughput and latency are measured, and how they hold up as the
dataset grows past the memory available to the process. This page describes
the method and the exact commands. Result tables are added only from runs on
a quiet machine, each next to the command and environment that produced it.

## The benchmark program

`kvlog_bench` (source `bench/kvlog_bench.cc`, built by default; CMake option
`KVLOG_BUILD_BENCH`) drives one database from a single thread. Run
`kvlog_bench --help` for every flag.

Keys are 16-byte zero-padded decimal numbers (`0000000000001234`), so byte
order equals numeric order. Values are `--value_size` bytes sliced from a
random buffer. Writes use `WriteOptions::sync = --sync` (default off: a write
survives a process crash, not a power loss).

| Benchmark | What it does |
|---|---|
| `fillseq` | Puts keys `0 .. num-1` in ascending order |
| `fillrandom` | Puts every key `0 .. num-1` once, in a scattered order (a fixed bijection, so every key exists afterwards) |
| `readrandom` | `--ops` Gets of existing keys |
| `readmissing` | `--ops` Gets of keys that were never written but sort between existing ones, so only the Bloom filter (not a table's key range) can skip the table |
| `mixed90`, `mixed50` | `--ops` operations, 90 % / 50 % Gets, the rest Puts overwriting existing keys |
| `mixed` | Same with `--read_pct` |

Reads and mixed writes pick keys with `--key_dist=uniform` or
`--key_dist=zipfian` (YCSB-style generator, `--zipf_theta=0.99` by default;
popular ranks are scattered over the key space with a 64-bit mix so the hot
keys are spread across all table files).

A fill is not finished until the flushes and compactions it caused are:
the timed phase ends with `DB::WaitForBackgroundWork()`.

### What each summary line reports

- **ops/s**: operations of the phase divided by its wall-clock time.
- **p50 / p99**: nearest-rank percentiles of per-operation latency
  (`steady_clock` around each call).
- **cache hit**: `block_cache_hits / (block_cache_hits + block_cache_misses)`
  over the phase (from `Options::statistics`).
- **bloom useful**: `bloom_useful / bloom_checks` over the phase: the share of
  table probes the filter answered without reading any block.
- **disk**: bytes of every file in the database directory (tables, logs,
  MANIFEST) at the end of the phase.
- **compactions**: compactions finished since the database was opened.
- `--verify=1` makes `readrandom` fail unless it found every key and
  `readmissing` fail if it found any (after a full fill of `--num` keys).

### Growth mode

`--growth=1` grows the dataset in steps of `--growth_step_bytes` logical bytes
(key + value) until `--growth_max_bytes`. Each step

1. inserts the next block of new keys in a scattered order and waits for
   background work,
2. runs `--ops` mixed operations (`--read_pct` reads, the rest overwrites)
   over all keys loaded so far, with the chosen key distribution,
3. writes one CSV row: `step, keys, dataset_bytes, disk_bytes, ops_per_sec,
   p50_us, p99_us, cache_hit_rate, bloom_useful_rate, compactions,
   block_cache_bytes, read_pct, key_dist`. All rates and latencies describe
   step 2 only.

## Past-memory runs

`scripts/run_memlimit_bench.sh` builds `kvlog_bench` (Release, g++) inside the
`kvlog-dev:latest` Docker image (built from `docker/Dockerfile`), then runs
growth mode in a container with
a cgroup memory limit, once per block-cache size:

    docker run --memory=512m --memory-swap=512m --cpus 2 ...
      kvlog_bench --db=/var/tmp/kvlog_bench --growth=1 ...

- The page cache is charged to the container's cgroup, so once the dataset is
  larger than the limit, reads that miss both the block cache and the
  container's share of the page cache go through to the Linux VM's virtual
  disk. On Docker Desktop that disk is itself a file on the host, so such a
  read may be served by the host's own file cache rather than the SSD: the
  penalty past the limit is real, but it is the cost of a virtual-disk read,
  not necessarily of a device read. Results therefore record how Docker's
  disk is stored and how much memory the VM has; a device-only claim needs a
  native Linux host.
- The database lives in the container's own file system (`/var/tmp`), not in
  the bind-mounted project directory, so its pages count against the limit.
- `--memory-swap` equals `--memory`: no swap.
- After each run the script prints the cgroup's `memory.peak`.

Defaults: 512 MiB limit, growth to 4 GiB in 256 MiB steps, 1000-byte values,
200 000 mixed ops per step, 90 % reads, zipfian keys, block caches of 8 MiB
and 128 MiB. Every setting is an environment variable (see the script
header). CSV files land in `bench/results/`, named after the limit, cache
size, distribution, read share and a UTC timestamp (the committed results
below drop the timestamp suffix).

Exact commands:

    # pipeline check only (seconds; not a measurement)
    SMOKE=1 scripts/run_memlimit_bench.sh

    # the measured run, on an otherwise idle machine
    scripts/run_memlimit_bench.sh
    MEMORY=512m CACHE_SIZES="8388608 134217728" READ_PCT=50 scripts/run_memlimit_bench.sh

    # Markdown tables from the CSV files
    python3 scripts/csv_to_markdown.py bench/results/growth-*.csv

## Environment to record with every result

Host CPU and core count, RAM, OS and kernel, Docker version, the memory and
CPUs given to Docker's VM, the image's compiler version, the container limits
(`--memory`, `--cpus`), how Docker's disk is stored (disk image type and the
host file system under it), and the full command line. Runs on a shared or busy
machine are not comparable with each other and are not reported here.

## Results

Six growth runs: three workloads, each with an 8 MiB and a 128 MiB block
cache, all under a 512 MiB cgroup limit, growing the dataset to 5.5 times
that limit. Each configuration was run once. The CSV files are in
`bench/results/`.

### Environment

- Host: MacBook Air (fanless), Apple M5, 10 cores (4 performance, 6
  efficiency), 16 GB RAM, macOS 26.5. `pmset -g therm` recorded no thermal
  or performance warning before or after the runs.
- Load: apart from a web browser, an editor and two light background tasks
  (web reading and code reading), the machine was idle. Load average
  (1/5/15 min) was 2.33 / 3.03 / 3.29 before the first run and 2.50 / 2.30 /
  2.64 after the last, on 10 cores.
- Docker Desktop 29.7.2 with default VM settings: 10 CPUs, 7934 MiB memory,
  Linux 7.0.12-linuxkit, cgroup v2, overlayfs storage driver. The VM's disk
  is a sparse raw image file (`Docker.raw`) on the host's APFS volume.
- Image `kvlog-dev:latest` from `docker/Dockerfile` (Ubuntu 24.04.5), g++ 13.3.0, CMake 3.28.3,
  `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`).
- Container: `--memory 512m --memory-swap 512m --cpus 2`; database in
  `/var/tmp` inside the container's file system.

### Commands

The script's defaults (growth to 4 GiB) were cut to 2.5 GiB to fit a time
budget; everything else is the script as checked in:

    export MEMORY=512m CPUS=2 MAX_BYTES=2684354560 STEP_BYTES=268435456 OPS=200000
    READ_PCT=90 KEY_DIST=zipfian CACHE_SIZES="8388608"   scripts/run_memlimit_bench.sh
    READ_PCT=90 KEY_DIST=zipfian CACHE_SIZES="134217728" scripts/run_memlimit_bench.sh
    READ_PCT=50 KEY_DIST=zipfian CACHE_SIZES="8388608 134217728" scripts/run_memlimit_bench.sh
    READ_PCT=90 KEY_DIST=uniform CACHE_SIZES="8388608 134217728" scripts/run_memlimit_bench.sh

    python3 scripts/csv_to_markdown.py bench/results/growth-*.csv

Inside the container each run is

    kvlog_bench --db=/var/tmp/kvlog_bench --growth=1 \
      --growth_max_bytes=2684354560 --growth_step_bytes=268435456 --ops=200000 \
      --read_pct=<90|50> --key_dist=<zipfian|uniform> --value_size=1000 \
      --block_cache_bytes=<8388608|134217728> --csv=... --stats=1

with the remaining flags at their defaults: `--bloom_bits=10`,
`--write_buffer_size=4194304`, `--sync=0`, `--zipf_theta=0.99`, `--seed=301`.
Each step adds 264,208 keys of 1,016 bytes (16-byte key, 1000-byte value).
Ten steps come to 2,684,353,280 bytes, just under `--growth_max_bytes`, so
an eleventh step runs and every run ends at 2,906,288 keys, 2,952,788,608
logical bytes (2816 MiB, 5.5 times the limit).

The six runs took 15 minutes 13 seconds of wall time in total, including
the (incremental) builds.

### Summary

Mixed-phase throughput (ops/s) as the logical dataset grows against the
512 MiB limit. "step 11 vs step 1" is the share of the in-memory throughput
kept at 5.5 times the limit; "step 11 vs step 4" is the share of the 2x
throughput kept after the dataset grew another 2.75 times.

| key dist | reads | block cache | 0.5x limit (step 1) | 1.0x (step 2) | 2.0x (step 4) | 5.5x (step 11) | step 11 vs step 1 | step 11 vs step 4 | p50 / p99 at 5.5x (us) | cache hits at 5.5x |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| zipfian | 90% | 8 MiB | 452,959 | 117,533 | 42,730 | 21,513 | 4.7% | 50.3% | 4.83 / 193.96 | 13.0% |
| zipfian | 90% | 128 MiB | 521,182 | 85,511 | 36,814 | 20,960 | 4.0% | 56.9% | 3.21 / 202.92 | 37.0% |
| zipfian | 50% | 8 MiB | 284,408 | 95,054 | 47,856 | 27,032 | 9.5% | 56.5% | 2.04 / 386.62 | 9.3% |
| zipfian | 50% | 128 MiB | 282,600 | 59,686 | 42,704 | 24,165 | 8.6% | 56.6% | 2.17 / 703.92 | 20.6% |
| uniform | 90% | 8 MiB | 237,932 | 64,308 | 14,865 | 9,660 | 4.1% | 65.0% | 117.00 / 289.38 | 0.3% |
| uniform | 90% | 128 MiB | 271,551 | 52,835 | 19,927 | 8,344 | 3.1% | 41.9% | 128.21 / 349.92 | 4.9% |

### What the numbers show

- **Where the limit is crossed.** At step 2 the logical dataset is 512 MiB
  and the database on disk is 535-575 MiB, so it no longer fits in the
  cgroup. `memory.peak` reached 536,870,912 bytes (the limit) in every run.
  Between step 1 (0.5x the limit) and step 2 throughput fell to 16-33 % of
  step 1 and p99 latency rose from 7-14 us to 132-184 us: reads that miss
  the block cache and the container's page cache now go to the VM's disk.
- **After the drop, the decline is gradual.** From 2x to 5.5x the limit
  the runs kept 42-65 % of their 2x throughput. At 5.5x the limit the
  zipfian runs still did 20,960-27,032 ops/s (4.0-9.5 % of their
  in-memory throughput) with a median latency of 2-5 us, because the hot
  keys stay in memory. Uniform keys with 90 % reads are the worst case:
  8,344-9,660 ops/s (3.1-4.1 %) with a median of 117-128 us, since most
  reads miss memory.
- **Past the limit, writes are cheaper than reads.** With 50 % reads the zipfian runs
  kept more throughput at 5.5x (27,032 and 24,165 ops/s) than with 90 %
  reads (21,513 and 20,960 ops/s): a put appends to the write-ahead log
  (no sync) and the memtable without reading any table. No run hit a
  write stop (`stops: 0` in `kvlog.stats`); the fills ran at 28,982-86,026
  ops/s per step (printed by `kvlog_bench` on stderr, not in the CSV).
- **A larger block cache raised the hit rate, not the throughput past the
  limit.** At 5.5x the 128 MiB cache lifted the hit rate from 13.0 to
  37.0 %, 9.3 to 20.6 % and 0.3 to 4.9 %, but throughput was slightly lower
  in all three pairs. While the dataset fit (step 1) it helped the 90 %
  read runs (521,182 vs 452,959 and 271,551 vs 237,932 ops/s). A likely
  reason, not measured separately: the block cache is charged to the same
  512 MiB cgroup and holds blocks that are also in the page cache, so a
  bigger cache leaves less page cache for the rest of the database.
- **Bloom filters.** 58.9-78.4 % of table probes were answered by the
  filter without reading a block, at every dataset size.
- **Disk use stayed near the data size.** Compactions (cumulative since open) reached
  1,533-1,984 by step 11, and the on-disk size stayed within 1.03-1.08
  times the logical dataset at the last step.

### Limits of these numbers

- **Docker Desktop.** The cgroup limit bounds the container's memory,
  including its page cache, so past the limit reads really leave the
  container's memory. But the VM's disk is a file (`Docker.raw`) on macOS,
  and the host keeps its own file cache outside the VM (16 GB RAM, larger
  than any dataset here), so some of those "disk" reads may have been
  served from host memory rather than the SSD. The measured penalty is
  the cost of a virtual-disk read, which may be lower than that of a real
  device read. A device-only figure needs a native Linux host.
- **One run per configuration, short phases.** Each step measures 200,000
  operations; at step 1 that is under one second. Step-to-step noise is
  visible (for example uniform, 128 MiB: 5,759 ops/s at step 9, then 8,311
  at step 10, with a p99 of 1149 us at step 9). Treat differences of a few
  percent as noise.
- **Single thread.** `kvlog_bench` issues operations from one thread;
  these are not multi-client numbers.

### Per-step tables

Produced by `scripts/csv_to_markdown.py` from the CSV files (headings one
level down). Dataset and on-disk sizes are in MiB against the 512 MiB
limit; "filter skips" is the bloom useful rate; compactions are counted
since the database was opened.

#### growth-mem512m-cache8388608-zipfian-r90.csv

| step | dataset (MiB) | on disk (MiB) | ops/s | p50 (us) | p99 (us) | cache hits | filter skips | compactions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 256.0 | 270.4 | 452,959 | 1.25 | 6.96 | 19.6% | 60.5% | 127 |
| 2 | 512.0 | 534.6 | 117,533 | 1.79 | 140.67 | 17.2% | 64.2% | 240 |
| 3 | 768.0 | 797.3 | 60,397 | 2.21 | 144.42 | 15.9% | 68.3% | 348 |
| 4 | 1024.0 | 1056.1 | 42,730 | 2.58 | 151.12 | 15.1% | 63.3% | 484 |
| 5 | 1280.0 | 1310.5 | 34,094 | 3.04 | 159.12 | 14.3% | 58.9% | 690 |
| 6 | 1536.0 | 1582.9 | 29,480 | 3.67 | 174.50 | 14.0% | 68.0% | 810 |
| 7 | 1792.0 | 1845.4 | 26,004 | 4.21 | 196.67 | 13.5% | 69.9% | 958 |
| 8 | 2048.0 | 2114.6 | 23,823 | 4.33 | 257.62 | 13.3% | 64.8% | 1086 |
| 9 | 2304.0 | 2371.4 | 23,314 | 4.58 | 206.38 | 12.8% | 66.4% | 1303 |
| 10 | 2560.0 | 2640.9 | 23,396 | 4.46 | 177.08 | 13.1% | 67.3% | 1422 |
| 11 | 2816.0 | 2912.1 | 21,513 | 4.83 | 193.96 | 13.0% | 71.7% | 1537 |

#### growth-mem512m-cache134217728-zipfian-r90.csv

| step | dataset (MiB) | on disk (MiB) | ops/s | p50 (us) | p99 (us) | cache hits | filter skips | compactions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 256.0 | 270.4 | 521,182 | 1.04 | 9.62 | 63.9% | 62.5% | 122 |
| 2 | 512.0 | 535.8 | 85,511 | 1.58 | 132.17 | 54.5% | 62.0% | 240 |
| 3 | 768.0 | 804.6 | 50,073 | 1.79 | 147.50 | 49.5% | 64.4% | 368 |
| 4 | 1024.0 | 1064.2 | 36,814 | 2.17 | 165.04 | 44.9% | 67.7% | 498 |
| 5 | 1280.0 | 1329.5 | 32,925 | 2.33 | 160.79 | 42.4% | 63.3% | 614 |
| 6 | 1536.0 | 1583.9 | 27,786 | 2.42 | 171.67 | 41.1% | 63.9% | 817 |
| 7 | 1792.0 | 1847.0 | 25,291 | 2.67 | 192.12 | 39.7% | 65.6% | 956 |
| 8 | 2048.0 | 2116.2 | 22,609 | 2.88 | 276.58 | 38.9% | 66.0% | 1070 |
| 9 | 2304.0 | 2385.7 | 22,531 | 2.88 | 243.79 | 37.8% | 67.9% | 1186 |
| 10 | 2560.0 | 2639.8 | 21,213 | 3.00 | 250.79 | 37.1% | 63.1% | 1411 |
| 11 | 2816.0 | 2909.5 | 20,960 | 3.21 | 202.92 | 37.0% | 70.3% | 1533 |

#### growth-mem512m-cache8388608-zipfian-r50.csv

| step | dataset (MiB) | on disk (MiB) | ops/s | p50 (us) | p99 (us) | cache hits | filter skips | compactions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 256.0 | 301.2 | 284,408 | 1.33 | 9.21 | 13.5% | 77.5% | 132 |
| 2 | 512.0 | 575.3 | 95,054 | 1.50 | 140.71 | 12.8% | 70.3% | 283 |
| 3 | 768.0 | 865.1 | 57,220 | 1.58 | 168.04 | 12.3% | 68.9% | 422 |
| 4 | 1024.0 | 1081.0 | 47,856 | 1.67 | 188.67 | 11.2% | 68.5% | 643 |
| 5 | 1280.0 | 1375.1 | 48,834 | 1.58 | 152.33 | 10.5% | 69.5% | 782 |
| 6 | 1536.0 | 1619.2 | 42,939 | 1.67 | 170.71 | 10.7% | 68.4% | 1002 |
| 7 | 1792.0 | 1909.8 | 37,433 | 1.79 | 218.92 | 9.9% | 70.3% | 1160 |
| 8 | 2048.0 | 2170.3 | 35,274 | 1.83 | 308.58 | 9.9% | 70.1% | 1384 |
| 9 | 2304.0 | 2493.8 | 31,152 | 1.88 | 310.50 | 9.9% | 71.2% | 1551 |
| 10 | 2560.0 | 2737.4 | 32,508 | 1.88 | 261.62 | 9.3% | 71.8% | 1751 |
| 11 | 2816.0 | 3025.7 | 27,032 | 2.04 | 386.62 | 9.3% | 72.3% | 1949 |

#### growth-mem512m-cache134217728-zipfian-r50.csv

| step | dataset (MiB) | on disk (MiB) | ops/s | p50 (us) | p99 (us) | cache hits | filter skips | compactions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 256.0 | 337.3 | 282,600 | 1.46 | 13.67 | 37.5% | 78.4% | 133 |
| 2 | 512.0 | 571.9 | 59,686 | 1.71 | 184.38 | 29.4% | 68.8% | 301 |
| 3 | 768.0 | 843.4 | 51,787 | 1.79 | 171.17 | 27.2% | 68.4% | 446 |
| 4 | 1024.0 | 1081.4 | 42,704 | 1.88 | 196.12 | 25.1% | 68.5% | 662 |
| 5 | 1280.0 | 1362.8 | 38,396 | 1.83 | 244.33 | 24.2% | 69.9% | 817 |
| 6 | 1536.0 | 1619.7 | 37,452 | 2.04 | 206.88 | 23.5% | 69.4% | 1038 |
| 7 | 1792.0 | 1942.6 | 35,656 | 1.92 | 246.21 | 22.4% | 70.3% | 1190 |
| 8 | 2048.0 | 2175.4 | 34,490 | 1.88 | 299.12 | 22.4% | 70.4% | 1410 |
| 9 | 2304.0 | 2504.8 | 29,401 | 2.04 | 354.12 | 21.9% | 71.1% | 1583 |
| 10 | 2560.0 | 2754.5 | 32,415 | 1.92 | 260.29 | 21.3% | 72.3% | 1785 |
| 11 | 2816.0 | 3027.1 | 24,165 | 2.17 | 703.92 | 20.6% | 71.6% | 1984 |

#### growth-mem512m-cache8388608-uniform-r90.csv

| step | dataset (MiB) | on disk (MiB) | ops/s | p50 (us) | p99 (us) | cache hits | filter skips | compactions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 256.0 | 276.6 | 237,932 | 3.88 | 9.29 | 3.1% | 65.6% | 123 |
| 2 | 512.0 | 548.0 | 64,308 | 5.21 | 155.62 | 1.5% | 71.5% | 233 |
| 3 | 768.0 | 818.4 | 34,541 | 9.04 | 172.29 | 1.1% | 68.3% | 388 |
| 4 | 1024.0 | 1053.5 | 14,865 | 30.50 | 248.96 | 0.8% | 62.3% | 590 |
| 5 | 1280.0 | 1330.7 | 14,135 | 57.50 | 233.83 | 0.6% | 68.1% | 716 |
| 6 | 1536.0 | 1607.1 | 13,832 | 85.88 | 230.62 | 0.5% | 73.7% | 844 |
| 7 | 1792.0 | 1853.5 | 11,792 | 105.96 | 230.08 | 0.6% | 66.8% | 1048 |
| 8 | 2048.0 | 2130.8 | 10,101 | 113.75 | 353.42 | 0.4% | 70.2% | 1176 |
| 9 | 2304.0 | 2401.8 | 9,908 | 114.83 | 312.71 | 0.4% | 73.7% | 1318 |
| 10 | 2560.0 | 2668.2 | 9,471 | 117.08 | 323.79 | 0.4% | 75.3% | 1546 |
| 11 | 2816.0 | 2946.4 | 9,660 | 117.00 | 289.38 | 0.3% | 76.2% | 1651 |

#### growth-mem512m-cache134217728-uniform-r90.csv

| step | dataset (MiB) | on disk (MiB) | ops/s | p50 (us) | p99 (us) | cache hits | filter skips | compactions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 256.0 | 276.4 | 271,551 | 3.83 | 10.08 | 43.3% | 66.4% | 123 |
| 2 | 512.0 | 547.1 | 52,835 | 5.71 | 137.83 | 23.1% | 68.5% | 237 |
| 3 | 768.0 | 821.2 | 38,744 | 23.58 | 175.08 | 15.6% | 65.9% | 379 |
| 4 | 1024.0 | 1070.0 | 19,927 | 25.54 | 215.29 | 11.9% | 71.5% | 512 |
| 5 | 1280.0 | 1318.1 | 12,547 | 103.38 | 226.29 | 9.6% | 69.1% | 725 |
| 6 | 1536.0 | 1594.9 | 12,365 | 104.50 | 237.00 | 8.4% | 69.4% | 841 |
| 7 | 1792.0 | 1872.4 | 11,655 | 107.33 | 254.75 | 7.2% | 70.9% | 955 |
| 8 | 2048.0 | 2123.3 | 8,646 | 119.00 | 358.88 | 6.2% | 70.4% | 1167 |
| 9 | 2304.0 | 2401.1 | 5,759 | 144.08 | 1149.04 | 5.8% | 72.9% | 1287 |
| 10 | 2560.0 | 2677.9 | 8,311 | 133.71 | 287.21 | 5.0% | 69.4% | 1417 |
| 11 | 2816.0 | 2938.4 | 8,344 | 128.21 | 349.92 | 4.9% | 75.0% | 1617 |
