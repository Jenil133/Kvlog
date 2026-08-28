# Kvlog design

Each section below describes one part of the engine. See also `src/table/format.h` and
`src/db/dbformat.h` for the byte-level formats.

## Overview

Kvlog is a LevelDB-style log-structured merge tree:

- Writes go to a write-ahead log (one record per group of batches) and then into an in-memory
  skiplist (the memtable). When the memtable passes `write_buffer_size` it is frozen, a new log
  is started, and the background thread writes the frozen memtable to a level-0 table.
- Tables (`<n>.sst`) are immutable sorted files with checksummed blocks, a per-table Bloom filter
  and an index. A `MANIFEST` log of `VersionEdit` records lists which tables make up the database;
  `CURRENT` names the live MANIFEST.
- The same background thread compacts levels (below) so that overwritten and deleted entries leave
  the disk and lookups probe few files.
- Reads look in the memtable, the frozen memtable, then the tables (level 0 newest first, then one
  file per deeper level). Point lookups check each table's Bloom filter first and go through a
  sharded LRU block cache.

| Source | Role |
|---|---|
| `src/db/db_impl.*` | open/recovery, write queue and group commit, flush, compaction driver, reads |
| `src/db/log_writer.*`, `log_reader.*` | WAL and MANIFEST record format, torn-tail vs damage |
| `src/db/memtable.*`, `skiplist.h`, `src/util/arena.*` | memtable |
| `src/db/version_set.*`, `version_edit.*` | versions, MANIFEST commits, compaction picking |
| `src/db/compaction.*`, `builder.*` | compaction inputs and output policy, table install |
| `src/table/*` | table builder/reader, blocks, filter block, iterators |
| `src/util/bloom.*`, `hash.*`, `cache.*` | Bloom filter, XXH64, block cache |
| `src/util/env_posix.cc` | file system access, syncs, locks |

## Write buffer and open files

- A memtable counts as full when the bytes allocated since it was created exceed
  `write_buffer_size` (`MemTable::BytesAdded`). An empty memtable already owns one 4 KiB arena
  block for the skiplist head. Comparing the raw arena size would make a buffer at the 4 KiB
  minimum "full" before its first entry, so every write would switch logs and flush a one-entry
  table.
- The table cache keeps one file descriptor per open table. `max_open_files` is clamped to the
  process's `RLIMIT_NOFILE` minus 64. Otherwise reads could fail with `EMFILE`, and so could
  creating the next log, table or MANIFEST, which would stop all writes.
- `GetChildren` checks `errno` after `readdir`, so an I/O error is reported instead of returning a
  truncated listing. Recovery relies on the listing to find logs and used file numbers.
- `DestroyDB` unlinks `LOCK` while still holding it. Unlocking first would let one process lock
  the old inode and another a new `LOCK`, so two processes could own the database.

## Files and formats

All numbered files share one counter, so a number is never reused.

| Name | Contents |
|---|---|
| `CURRENT` | name of the live MANIFEST and a newline; replaced by temp file -> sync -> rename -> directory sync |
| `LOCK` | `flock` while the database is open |
| `MANIFEST-<n>` | log of `VersionEdit` records; a new one starts with a snapshot of all files |
| `<n>.log` | write-ahead log |
| `<n>.sst` | table |
| `<n>.tmp` | table being written, or a CURRENT replacement; never referenced |
| `lost/` | logs that point-in-time recovery could not fully replay, with a `RECOVERY-<seq>.txt` note |

- Log record: 13-byte header `masked crc32c(header[4..13)) | masked crc32c(payload) | length |
  type` and the payload. The header has its own checksum so a damaged length is detected rather
  than read as a record running past the end of the file.
- WAL payload: the write batch, `sequence fixed64 | count fixed32 | (tag key [value])*`.
- MANIFEST payload: one `VersionEdit`, a sequence of `varint32 tag | field` (1 comparator, 2 log
  number, 3 next file number, 4 last sequence, 5 reserved, 6 deleted file, 7 new file) closed by
  tag 127. That end marker is a single non-zero byte (0x7f), so an encoded edit never ends in a
  zero byte; see "How a log ends" for why that matters. An edit without it, or with bytes after
  it, does not decode.
- Internal key: `user key | fixed64 (sequence << 8 | type)`, ordered by user key ascending, then
  sequence descending.
- Table: `[data blocks][filter block][metaindex][index][footer]`, every block followed by a
  5-byte trailer (type + masked CRC-32C). Data blocks use prefix compression with restart points;
  the index maps the last key of each data block to its handle; the metaindex maps
  `filter.kvlog.bloom64` to the filter block; the 52-byte footer ends with the magic
  `"KVLOGTB1"`.

## Durability and recovery

### Commit protocol

- A write group is appended to the log as one record; with `sync` it is synced before it is
  acknowledged. Any log failure is sticky: nothing is ever appended after a possibly torn record.
- Switching memtables syncs the retiring log, creates the new log and syncs the directory before
  the new log takes writes. A newer log is therefore never durable while part of an older one is
  not.
- A flush or compaction output is written as `<n>.tmp`, synced, renamed to `<n>.sst`, the
  directory synced, and the table reopened to verify it. Only then does a MANIFEST edit refer to
  it. The edit is synced before the new version is installed and before any file it obsoletes is
  deleted.
- A new MANIFEST (the first update after open, or past `max_manifest_file_size`) starts with a
  snapshot and is synced before CURRENT is switched to it. If an update fails once the CURRENT
  rename may have happened, the new MANIFEST is kept (it may be live) and the old one is never
  appended to again; the next update starts yet another MANIFEST.
- Any failed update sets a sticky error, which also stops file deletion: whether the MANIFEST
  change reached the disk is unknown until a reopen. This includes a compaction whose install
  fails while the database is closing; only a compaction abandoned before its install (its
  outputs removed, the MANIFEST untouched) is not an error.
- `create_if_missing` syncs the parent directory after creating the database directory. On macOS
  `F_FULLFSYNC` falls back to `fsync` only when the file system does not support it; any other
  failure is reported.

### How a log ends

`log::Reader` distinguishes the ways a log (WAL or MANIFEST) can end:

- clean end after a complete record;
- torn tail: the file ends inside a header or payload, or a record fails a checksum because the
  file ends in a run of zero bytes that starts inside it. Zero fill is what file systems that
  extend a file before writing its data leave after power loss. The run must start at the record
  start, at a 512-byte boundary, or be at least 32 bytes long. That excludes most records whose
  own content ends in a few zero bytes and then had a bit flipped elsewhere.
- corruption: any other checksum failure, including a complete final record whose bytes changed.
  Such a record may have been an acknowledged synced write, or a MANIFEST edit that files were
  already deleted on the strength of, so it is never dropped silently.

The zero-fill rule cannot be exact. A damaged final record whose own payload ends in zero bytes
is read as a torn tail when that run of zeros reaches 32 bytes, or starts on a 512-byte boundary
(about one record in 512 of those ending in a few zeros). Two consequences:

- MANIFEST: not affected. Every edit ends in the non-zero end marker, so a damaged final edit has
  no trailing zeros to explain it and always fails the open
  (`VersionSetTest.DamagedFinalEditIsCorruptionAtEveryAlignment` damages the final edit at all
  512 alignments; without the marker one of them passed as a torn tail).
- WAL: a write batch ends in its last value, which can end in zero bytes. A damaged final WAL
  record whose value ends that way can be dropped as a torn tail, losing that one write even with
  `paranoid_checks` (`LogTest.DamagedFinalRecordWithZerosFromASectorBoundaryIsATornTail` pins the
  behaviour). Records before it are unaffected.

### Recovery (`DB::Open`)

1. Create the directory if allowed, take `LOCK`. A missing CURRENT next to logs or tables is
   corruption; otherwise a missing CURRENT means a new database.
2. Replay the MANIFEST: a torn final record is an edit that never committed and is ignored;
   corruption anywhere fails the open.
3. Replay the logs at or above the MANIFEST's log number, oldest first, into level-0 tables.
   - A torn tail of the last log, or of a log followed only by empty logs (a crash during an earlier
     open), is dropped.
   - A torn tail followed by a log with data is damage: every log is synced before the next one
     takes writes, so a sync did not reach the disk.
   - On damage, `paranoid_checks` fails the open and changes nothing on disk. Otherwise replay stops
     at the damage and skips every later log (point-in-time recovery: never a later write without an
     earlier one). The damaged and skipped logs are copied to `lost/` with a
     `RECOVERY-<seq>.txt` note, each synced, and `lost/` and the database directory are synced,
     all before the MANIFEST edit that retires the originals; they are deleted only after that
     edit commits. A crash at any point therefore leaves each log in the database directory or in
     `lost/`. If the copies cannot be made, the open fails. `kvlog.stats` reports what happened.
4. Start a new log, commit the replayed tables and the new log number in one MANIFEST edit, then
   delete obsolete files.

### Testing durability

- Kill tests (`Crash testing` below) cover process crashes: everything handed to the OS
  survives.
- Power-loss tests cover the stronger promise of `sync`. `tests/power_loss_env.h` models a POSIX
  file system: file data survives only up to its last successful `Sync()`, directory entries only
  as of the last `SyncDir()`.
  - `PowerLossTest` stops the machine at every durability operation (sync, directory sync,
    rename, create, unlink) of a workload that writes with and without `sync`, flushes, switches
    memtables, compacts, rolls the MANIFEST over and reopens.
  - After each stop it rebuilds the directory from what was durable and opens it with
    `paranoid_checks`.
  - The contents must equal the state after some prefix of the operations that includes every
    write acknowledged with `sync`, and every write acknowledged before a completed `Flush()` or
    clean close.
  - Deleting the table sync, or the retiring-log sync in the memtable switch, makes it fail.
- `FaultInjectionTest.FlushFollowsTheDurableCommitProtocol` asserts the exact order of a flush's
  durability operations. `FailedCurrentUpdateDuringARolloverLosesNothing` fails each step of a
  CURRENT update during MANIFEST rollovers.

## Bloom filters and the block cache

### Hash

`src/util/hash.{h,cc}` implements XXH64 (64-bit xxHash) in-tree. The filter is stored on disk, so the
hash is part of the file format and must not depend on platform, compiler or process (`std::hash` does).
Input is read little-endian on every host. `tests/hash_test.cc` checks the published XXH64 values
(`""`, `"a"`, `"abc"`, the 43-byte "quick brown fox" string) plus longer inputs. All of them were also
checked against zstd's frame checksum, which is the low 32 bits of XXH64 with seed 0, for every length
from 0 to 130. The test also pins a digest over 301 lengths, so any change to the hash fails it.

### Filter

`src/util/bloom.{h,cc}`: encoded filter = `bit array | k (u8)`. Bits = `keys * bits_per_key`, at least 64,
rounded up to whole bytes. `k = round(bits_per_key * ln 2)`, clamped to [1, 30] (10 bits/key gives k = 7).
Each key is hashed once (`Hash64(key, 0)`). The low and high 32-bit halves `h1`, `h2` drive double
hashing: probe j tests bit `(h1 + j*h2) mod bits`. A filter that is too short or has an unknown k
matches every key, so a filter can only ever save work and never hide a key.

Measured with `bloom_test` (100k keys, 100k absent probes, 10 bits/key, six key shapes: decimal strings,
little/big-endian integers, random binary, long common prefix, varints): false-positive rate 0.79% to
0.88%. The test asserts < 2% and no false negatives.

### Filter block in tables

There is one filter per table, built over the **user keys** (versions of a key are added once).
I chose this over LevelDB's one filter per 2 KiB of data because the filter is checked before the
index: a negative answer then costs no index search and no block read. The cost is that the whole
filter stays in memory while the table is open (about `keys * bits_per_key / 8` bytes, e.g. 25 KiB for
20k keys).

Layout: `[data blocks][filter block][metaindex][index][footer]`. The filter block has the usual
checksummed 5-byte trailer. The metaindex maps `filter.kvlog.bloom64` to its handle. The footer and
format version are unchanged, so tables written without a filter (all phase 1 tables, or
`bloom_bits_per_key = 0`) still open: they have no metaindex entry and are read without a filter.
Unknown metaindex names are ignored. A damaged filter block fails `Table::Open` with Corruption, the
same as a damaged index. `TableBuilder` writes the filter when `bloom_bits_per_key > 0`. Flushes and
compactions both go through it, so every new table gets one.

Lookup order in `Table::InternalGet`: filter -> index -> one data block. Statistics:
- `bloom_checks`: tables whose filter was consulted.
- `bloom_useful`: lookups the filter rejected.
- `bloom_false_positives`: the filter passed the key, but the table holds no entry for that user key.

`table_test` (`TableFilterDBTest.MissingKeyLookupsRarelyTouchADataBlock`): 5 interleaved level-0
tables, each spanning the whole key range, with 10,000 missing keys that fall between stored keys.
Result: 50,000 filter probes and 432 data blocks read (0.86%). The test asserts <= 2%.

### Block cache

`src/util/cache.{h,cc}`: a sharded LRU cache with a byte capacity. Entries carry a key, an opaque value,
a charge and a deleter. `Lookup`/`Insert` return ref-counted handles. A pinned entry is never freed:
if it is evicted or erased it only becomes unfindable, and it is freed when its last handle is
released. Only unpinned entries are evicted. The total may exceed the capacity while handles are held,
and is trimmed back as they are released. Shards: up to 16, each with its own mutex and an equal share
of the capacity. Small caches use fewer shards so that each shard keeps at least 256 KiB and can still
hold a few blocks. The cache keeps hit/miss/insert/eviction counters, and deleters run outside the
shard lock.

`TableCache` creates one cache per database from `block_cache_capacity` (0 = no cache) and passes it to
every table it opens. Key = `fixed64 file number | fixed64 block offset`. File numbers are never reused
in a database, so a key never names stale bytes. A table that is closed and reopened by the table cache
finds its blocks still cached. Blocks of deleted tables are never looked up again and age out. Charge =
block size + `sizeof(Block)`. A cached block is handed out as a `shared_ptr<const Block>` whose deleter
releases the cache handle, so an iterator or lookup still using a block keeps it pinned.

**Which reads use the cache:** point lookups (`Get`) look in the cache and fill it on a miss. Iterators,
including compaction inputs, read data blocks directly and neither look up nor fill the cache. A scan
or compaction touches every block once, and filling the cache with those blocks would evict the working
set that repeated point lookups depend on. `block_reads`/`block_read_bytes` count every data block read
from a file (cache misses, uncached lookups, iterator reads). `block_cache_hits`/`block_cache_misses`
count point-lookup cache probes.

`kvlog.stats` adds a `block cache:` line (capacity, bytes in use, hits, misses, evictions)
from the cache's own counters, and `Statistics::ToString()` when `Options::statistics` is set.

`CompactionTest.CompactionOutputsCarryFiltersAndReadsHitTheBlockCache` checks that the features
work together. After `CompactRange` every table is a compaction output. Lookups of missing keys
that sort between stored keys are answered by those tables' filters, with at most 2% reaching a
data block. A second pass over 500 present keys reads no block from a file and adds exactly 500
cache hits. `kvlog.stats` shows the cache line and the counters.
Measured (`build/debug/tests/kvlog_tests
--gtest_filter='CompactionTest.CompactionOutputsCarryFiltersAndReadsHitTheBlockCache'`, macOS arm64,
`debug` preset): 2000 missing-key lookups made 1978 filter checks, 1963 rejected by the filter and 15
false positives, so 15 data blocks were touched (0.76% of checks). Two passes of 500 present-key reads:
44 block reads and 44 cache misses (all in the first pass), 956 cache hits.

## Crash testing

Recovery is tested by killing a real process, not by simulating a crash
inside the test. `tests/crash_helper.cc` builds `kvlog_crash_helper`, which
opens a database and writes until it receives SIGKILL: no flush, no close,
no destructors run.

- **Op stream.** `random-writes` applies a deterministic stream of puts and
  deletes over 500 keys (`tests/crash_ops.h`; op *n* is the same for a given
  seed on every platform). Each value names the op that wrote it.
- **Acknowledgements.** After the database acknowledges an op, the helper
  appends one line for it to an ack file (one `write()` per line, so a kill
  can cut off at most the last line) and only then announces it on stdout.
  An op is therefore either recorded or still in flight; never the reverse.
- **Engine settings** travel as `name=value` arguments (`l0_trigger`,
  `level_base`, `target_file`, `throttle_us`, `no_auto_compactions`), and the
  helper prints the flush and compaction counts of its `Statistics` with every
  ack, so a test knows how much background work the killed process did.
- **Verification** (`tests/crash_test_util.h`). After a kill the test reopens
  the database and checks every key of the stream: it holds the result of
  its last acknowledged op, or the result of the single op after the last
  acknowledged one (the op in flight may or may not have reached the log).
  Any other value, any key outside the stream, an unsorted scan or a Get that
  disagrees with the scan fails the test.
- **Killing inside a compaction.** Kills at random acks land inside flushes and
  compactions only by chance, so `compact-and-die` makes it certain: it loads
  data with automatic compactions off, flushes, then runs `CompactRange` over
  everything. Its Env makes the compaction's *n*-th append to an output file
  (`*.tmp`, counted from the start of the compaction; *n* is chosen by the
  test) block forever and reports the file; the test then kills the helper.
  The kill point is therefore fixed by the data, not by how fast the
  compaction runs. After the kill the compaction's inputs must still be in
  level 0 (the edit never committed), the recovered contents must equal the
  model exactly, and no partial output may remain.
- **Settling.** With `settle_after=<n>` the helper waits for its background
  flushes and compactions to finish after acknowledging op *n*, before
  announcing it. A test that asserts "the killed process had finished a
  compaction" uses it, so the assertion depends on the data, not on
  scheduling; the kill still comes later, at a random point.
- **Strict reopen.** A process kill is never damage, so the kill loops reopen
  with `paranoid_checks` on and check that `kvlog.stats` has no `recovery:`
  line. A kill tail misread as damage would fail the open instead of being
  cut away quietly by point-in-time recovery.

Tests: `CrashTest.*` (default suite) and, labelled `stress`,
`StressTest.CompactionCrashLoopKeepsEveryAcknowledgedOp` (120 kills on one
database, every op synced, 16 KiB write buffer, level-0 trigger 2, 24 KiB
level-1 target; a third of the rounds use 200 us and a third 1000 us of
compaction throttle; every 20th round uses real file-system syncs; the other
rounds settle after op 450 and must have finished a compaction) and
`StressTest.KillsInsideManualCompactionsLoseNothing` (30 kills inside a
manual compaction, at appends 1 to 10). Most rounds use `NoSyncEnv`, whose
syncs hand data to the OS without forcing it to the device: identical for a
process crash, and far faster than `F_FULLFSYNC` on macOS.

Measured on fresh builds (the counts vary with timing, but the settled rounds
make "finished a compaction" certain): one run of the compaction crash loop
printed `120 kills, 141955 acknowledged ops verified; helpers finished 958
flushes and 451 compactions (115 rounds compacted); 104 kills interrupted a
table write`. Across the macOS `debug`, `asan` and `tsan` builds and the Linux
g++ 13.3 and clang 18 (ASan) builds, the same loop finished between 451 and
544 compactions. `KillsInsideManualCompactionsLoseNothing` reports 30 of 30 kills inside
the compaction by construction.

## Compaction

Leveled compaction in the LevelDB style (`src/db/compaction.*`, picking in
`src/db/version_set.cc`, the merge in `DBImpl::DoCompactionWork`).

### Shape of the tree

- Level 0 holds memtable flushes; its files may overlap, and a higher file
  number means newer data.
- Every level n >= 1 is a sorted run of files with disjoint user-key ranges.
  Compaction outputs are cut only between two user keys, so a user key never
  spans two files of a level; `VersionSet::LogAndApply` rejects any edit that
  would make files of a level >= 1 overlap.
- For any user key, newer versions live in shallower levels.

### When and what to compact

Every new version gets a score (`VersionSet::Finalize`):

- level 0: `files / level0_compaction_trigger` (file count, because every
  level-0 file may be probed by a lookup);
- level n in 1..5: `bytes / (max_bytes_for_level_base *
  max_bytes_for_level_multiplier^(n-1))`;
- level 6 (the last) is never compacted.

The background thread compacts while the best score is >= 1. Inputs:

- level 0: the oldest file, plus every level-0 file overlapping the growing
  user-key range (transitively), so all level-0 versions of those keys are
  merged together;
- level n >= 1: one file, chosen round robin through the key space (an
  in-memory compaction pointer per level, the largest key of the previous pick;
  not persisted, so after a restart the rotation starts over);
- plus every file of the next level that overlaps the inputs' range.

A single input file with nothing to merge with in the next level, and not
overlapping more than `10 * target_file_size` of the level below that, is
moved down by a MANIFEST edit alone (a "trivial move").

### The merge

A merging iterator over the inputs yields entries in internal-key order
(user key ascending, sequence descending). For each user key:

- the first (newest) entry is kept and every older one is dropped. There is no
  snapshot API, and every open iterator pins the version it started from, so
  it keeps reading the old input files; nobody needs the old versions in the
  new files;
- a tombstone is dropped if no file in a level below the output level covers
  the key (`Compaction::IsBaseLevelForKey`); otherwise it stays, because an
  older value further down would come back to life without it.

Outputs are written with `TableBuilder` (so they get Bloom filters like any
table), cut when they reach `target_file_size` or overlap more than
`10 * target_file_size` of the level below the output, and installed as
`<n>.tmp` -> fsync -> rename -> directory fsync -> reopen check, exactly like a
flush (`InstallTableFile` in `src/db/builder.cc`). Output numbers sit in
`pending_outputs_` until the MANIFEST lists them.

One `VersionEdit` then removes every input and adds every output; it is
synced to the MANIFEST before the new version is installed. After a crash the
database therefore has either all old files or all new ones; unreferenced
outputs are deleted by the next open.

### Threads and file lifetime

One background thread does flushes and compactions, so `LogAndApply` has a
single caller. Flushes take priority: between entries a running compaction
checks an atomic "immutable memtable exists" flag and flushes it on the spot
(the new level-0 file is newer than anything the compaction reads, so the
merge is unaffected). The DB mutex is released while merging and writing.

A compaction holds a reference to the version it picked its inputs from;
readers hold the version they started with. A table file is deleted only once
no live version lists it (`VersionSet::AddLiveFiles` walks the weak references
to every version still alive), so open iterators keep their files. Deletion
runs after each flush or compaction; a file freed by the last iterator going
away is removed by the next one.

### Write stalls

With automatic compaction on, the writer that needs to switch memtables:

- sleeps about 1 ms once per write group while level 0 has
  `>= level0_slowdown_writes_trigger` files (`Statistics::write_slowdowns`),
  spreading the cost of compaction over many writes;
- blocks while level 0 has `>= level0_stop_writes_trigger` files
  (`Statistics::write_stops`) until a compaction finishes.

With `disable_auto_compactions` the triggers are ignored (nothing would ever
shrink level 0). Options are clamped so that compaction trigger <= slowdown
trigger <= stop trigger: a stop is never reached with no compaction pending.

### Public API and options

- `DB::CompactRange(begin, end)` flushes the memtable, then compacts the range
  level by level down to the deepest level holding part of it, always
  rewriting (never a trivial move), so overwritten and deleted entries in the
  range leave the disk. It runs on the background thread and the caller waits;
  it works with `disable_auto_compactions` set.
- `DB::WaitForBackgroundWork()` blocks until no flush or compaction is pending
  or running (or background work failed).
- `compaction_throttle_us` sleeps after every data block a compaction writes.
- `kvlog.stats` lists files, bytes, compactions, trivial moves and bytes
  read/written per level, then `Statistics::ToString()` when
  `Options::statistics` is set. Counters: `flushes`, `compactions` (merges, not
  trivial moves), `compaction_bytes_read`, `compaction_bytes_written`,
  `write_slowdowns`, `write_stops`.
- Clamping (`SanitizeOptions` in `src/db/db_impl.cc`):
  `level0_compaction_trigger` [1, 1000], slowdown [trigger, 1000], stop
  [slowdown, 1000], `max_bytes_for_level_base` [1 KiB, 1 TiB],
  `max_bytes_for_level_multiplier` [2, 100], `target_file_size`
  [1 KiB, 1 GiB], `compaction_throttle_us` [0, 1 s], `bloom_bits_per_key`
  [0, 64], `block_cache_capacity` [0, 1 TiB].

### Space amplification (measured)

`SpaceAmpTest` (`tests/space_amp_test.cc`): 60,000 operations over 3,000 keys
(3/4 puts of 100-byte random values, 1/4 deletes; puts total 19.9x the final
live data), `write_buffer_size` 32 KiB, `target_file_size` 32 KiB,
`max_bytes_for_level_base` 64 KiB, multiplier 10, level-0 trigger 4. Disk
bytes = all `.sst` plus `.log` files, measured after `WaitForBackgroundWork()`
at each quarter of the workload. Command:
`build/debug/tests/kvlog_tests --gtest_filter='SpaceAmpTest.*'`
(macOS arm64, Apple clang 21, `debug` preset, `NoSyncEnv`):

    with compaction: live 245795 B, written 4899223 B (19.9x live), on disk at each quarter: 320791 B (1.31x) 347924 B (1.42x) 378760 B (1.54x) 406109 B (1.65x)
    without compaction: live 245795 B, written 4899223 B (19.9x live), on disk at each quarter: 1366056 B (5.56x) 2732028 B (11.12x) 4096791 B (16.67x) 5457155 B (22.20x)
    after CompactRange: 261018 B (1.06x live)

The "without compaction" numbers do not depend on timing. With compaction they
depend on how the background thread's work interleaved with the writes; an
earlier run of the same test (before the write-buffer accounting fix in "Write
buffer and open files") printed 1.24x 1.31x 1.36x 1.48x, and ten further runs of the `release` build
stayed between 1.22x and 1.71x at every checkpoint. The test asserts
<= 3x at every checkpoint with compaction, and strictly growing, > 10x live at
the end, without it.
