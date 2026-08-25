#pragma once

// Record format shared by the write-ahead log and the MANIFEST.
//
// A log file is a sequence of records, each a 13-byte header followed by the
// payload:
//
//   offset 0   fixed32  header_crc   masked CRC-32C of bytes [4, 13)
//   offset 4   fixed32  payload_crc  masked CRC-32C of the payload
//   offset 8   fixed32  length       payload size in bytes
//   offset 12  uint8    type         RecordType
//   offset 13  payload  (length bytes)
//
// The header has its own checksum so that a damaged length field is reported
// as corruption instead of being mistaken for a record that runs past the end
// of the file (which is what a torn write looks like). See log_reader.h for
// how the reader classifies the end of a log.

#include <cstddef>
#include <cstdint>

namespace kvlog::log {

enum RecordType : uint8_t {
  // Never written. Reserved so that a zero-filled region (left by some file
  // systems when a crash cuts off unsynced appends) never parses as a record.
  kZeroType = 0,
  // A complete payload: one write batch in the WAL, one version edit in the
  // MANIFEST.
  kFullType = 1,
};

inline constexpr uint8_t kMaxRecordType = kFullType;

inline constexpr size_t kHeaderSize = 4 + 4 + 4 + 1;

// The length field is 32 bits wide.
inline constexpr uint64_t kMaxPayloadSize = 0xffffffffu;

}  // namespace kvlog::log
