#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "kvlog/statistics.h"

namespace kvlog {

class Env;

// Options fixed for the lifetime of an open database. Out-of-range numeric
// values are clamped into the documented ranges by DB::Open.
struct Options {
  // Create the database if it does not exist.
  bool create_if_missing = false;

  // Fail DB::Open if the database already exists.
  bool error_if_exists = false;

  // Controls what DB::Open does when a write-ahead log is damaged.
  //
  // A torn tail, the normal result of a crash, is not damage and is always
  // dropped silently: the last log ends part way through a record, or in a
  // run of zero bytes left by power loss. Damage is anything else: a record
  // that fails its checksum or does not decode, even the last complete one
  // (it may be an acknowledged synced write), and a torn tail in a log that
  // is followed by a newer log holding data (a sync that never reached the
  // disk).
  //
  // When true, DB::Open fails with Corruption and changes nothing on disk.
  // When false (the default), recovery is point-in-time: it keeps every
  // write before the damage and none after it, in that log or any later
  // one, so the database reflects a consistent prefix of the write history.
  // The damaged and skipped logs are copied into the lost/ subdirectory with
  // a RECOVERY-<sequence>.txt note before the originals are retired, and the
  // "kvlog.stats" property reports what happened. If they cannot be copied,
  // DB::Open fails instead.
  //
  // Damage to the MANIFEST always fails DB::Open, whatever this says.
  bool paranoid_checks = false;

  // Bytes of updates buffered in the in-memory table before it is frozen and
  // flushed to a sorted table file. Range [4 KiB, 1 GiB].
  size_t write_buffer_size = 4 * 1024 * 1024;

  // Approximate size of the user data packed into one table data block.
  // Range [256 B, 4 MiB].
  size_t block_size = 4 * 1024;

  // Number of keys between restart points in a data block; keys in between
  // are prefix-compressed against their predecessor. Range [1, 1024].
  int block_restart_interval = 16;

  // Maximum number of table files kept open by the table cache.
  // Range [16, 50000].
  int max_open_files = 1000;

  // When the MANIFEST grows beyond this many bytes, the next update writes a
  // compact snapshot of the file set into a new MANIFEST instead of appending.
  // Range [1 B, 1 GiB].
  uint64_t max_manifest_file_size = 4 * 1024 * 1024;

  // --- Compaction -----------------------------------------------------------

  // Number of level-0 files that triggers a compaction into level 1.
  int level0_compaction_trigger = 4;

  // Level-0 file counts at which writes are first delayed (about 1 ms each)
  // and then blocked until compaction catches up.
  int level0_slowdown_writes_trigger = 8;
  int level0_stop_writes_trigger = 12;

  // Target total size of level 1. Each deeper level may hold
  // max_bytes_for_level_multiplier times more than the one above it.
  uint64_t max_bytes_for_level_base = 10 * 1024 * 1024;
  int max_bytes_for_level_multiplier = 10;

  // Compaction output is split into table files of about this size.
  uint64_t target_file_size = 2 * 1024 * 1024;

  // Turns background compaction off; memtables are still flushed to level 0.
  // Useful for measuring how disk use grows without compaction.
  bool disable_auto_compactions = false;

  // Pause inserted after each data block a compaction writes. Rate-limits
  // background I/O, and lets tests widen the window in which a crash lands in
  // the middle of a running compaction. 0 means no pause.
  uint32_t compaction_throttle_us = 0;

  // --- Reads ----------------------------------------------------------------

  // Bloom filter bits stored per key in every table file; about 10 bits gives
  // a 1% false-positive rate. 0 disables filters.
  int bloom_bits_per_key = 10;

  // Capacity in bytes of the LRU cache of data blocks shared by all table
  // files. 0 disables the cache.
  size_t block_cache_capacity = 8 * 1024 * 1024;

  // Optional counters updated by the database (filter checks, block reads,
  // cache hits, flushes, compactions). nullptr disables collection.
  std::shared_ptr<Statistics> statistics;

  // File system access. nullptr means Env::Default(). Not owned; must outlive
  // the database.
  Env* env = nullptr;
};

// Options for reads (Get and iterators). Every read observes a consistent
// snapshot: the state of the database when the read (or iterator) started.
struct ReadOptions {};

struct WriteOptions {
  // If true, the write-ahead log is forced to stable storage (fdatasync on
  // Linux, F_FULLFSYNC on macOS) before the write is acknowledged, so the
  // write survives power loss. If false, the record is handed to the
  // operating system before acknowledging: it survives a process crash but
  // the last few writes may be lost if the machine itself goes down.
  bool sync = false;
};

}  // namespace kvlog
