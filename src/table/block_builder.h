#pragma once

// Builds a block of sorted key/value entries with prefix compression.
//
//   entry := shared (varint32) | non_shared (varint32) | value_length (varint32)
//            | key_delta (non_shared bytes) | value (value_length bytes)
//   block := entry* | restart_offset (fixed32)* | num_restarts (fixed32)
//
// Each key is stored as the number of bytes it shares with the previous key
// plus the remaining suffix. Every `restart_interval` entries the full key is
// stored (shared = 0) and its offset recorded in the restart array, so a
// reader can binary-search the restart points and then scan forward.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kvlog {

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval);
  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Starts a new, empty block.
  void Reset();

  // Requires: Finish() not called since the last Reset(), and `key` greater
  // than every key added so far.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block, which stays
  // valid until Reset() or destruction.
  std::string_view Finish();

  // Size of the block if it were finished now.
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;  // Entries since the last restart point.
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace kvlog
