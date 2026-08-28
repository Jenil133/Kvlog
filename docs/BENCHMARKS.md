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
`everpure-dev:latest` Docker image, then runs growth mode in a container with
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
size, distribution, read share and a UTC timestamp.

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

No measured results yet. Timed runs are made in a separate quiet-machine
step; until then this section stays empty rather than holding numbers from a
loaded machine.
