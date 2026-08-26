#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "db/dbformat.h"
#include "db_test_util.h"
#include "kvlog/statistics.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/table.h"
#include "table/table_builder.h"
#include "test_util.h"
#include "util/cache.h"
#include "util/coding.h"
#include "util/comparator.h"

namespace kvlog {
namespace {

using Entry = std::pair<std::string, std::string>;  // (key, value)

BlockContents CopyToContents(std::string_view bytes) {
  BlockContents contents;
  // Exactly the block's size, so that the sanitizer builds catch any read
  // past its end.
  contents.heap = std::make_unique<char[]>(bytes.size());
  std::memcpy(contents.heap.get(), bytes.data(), bytes.size());
  contents.data = std::string_view(contents.heap.get(), bytes.size());
  return contents;
}

std::vector<Entry> ScanIterator(Iterator* iter) {
  std::vector<Entry> result;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    result.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Blocks

class BlockRestartIntervalTest : public testing::TestWithParam<int> {};

TEST_P(BlockRestartIntervalTest, RoundTripsAndSeeksWithPrefixCompression) {
  const int restart_interval = GetParam();
  std::mt19937_64 rng(static_cast<uint64_t>(restart_interval));
  // Keys share long prefixes, so most entries are stored as deltas.
  std::set<std::string> keys;
  while (keys.size() < 500) {
    keys.insert("user/" + std::to_string(rng() % 50) + "/item/" +
                test::RandomBytes(&rng, rng() % 6));
  }
  std::vector<Entry> entries;
  for (const std::string& key : keys) {
    entries.emplace_back(key, test::RandomBytes(&rng, rng() % 40));
  }

  BlockBuilder builder(restart_interval);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  const size_t estimate = builder.CurrentSizeEstimate();
  const std::string contents(builder.Finish());
  EXPECT_EQ(estimate, contents.size());
  Block block(CopyToContents(contents));

  auto iter = block.NewIterator(BytewiseComparator());
  EXPECT_EQ(entries, ScanIterator(iter.get()));
  EXPECT_TRUE(iter->status().ok());

  for (size_t i = 0; i < entries.size(); ++i) {
    iter->Seek(entries[i].first);
    ASSERT_TRUE(iter->Valid());
    EXPECT_EQ(entries[i].first, iter->key());
    EXPECT_EQ(entries[i].second, iter->value());
    // Appending a zero byte gives the smallest key above key i, so seeking
    // to it lands on key i + 1.
    iter->Seek(entries[i].first + std::string(1, '\0'));
    if (i + 1 < entries.size()) {
      ASSERT_TRUE(iter->Valid());
      EXPECT_EQ(entries[i + 1].first, iter->key());
    } else {
      EXPECT_FALSE(iter->Valid());
    }
  }
  iter->Seek("");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(entries.front().first, iter->key());
  iter->Seek("zzz");
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

INSTANTIATE_TEST_SUITE_P(RestartIntervals, BlockRestartIntervalTest,
                         testing::Values(1, 2, 3, 16, 1024));

TEST(BlockBuilderTest, EmptyBlockHasNoEntries) {
  BlockBuilder builder(16);
  EXPECT_TRUE(builder.empty());
  Block block(CopyToContents(builder.Finish()));
  auto iter = block.NewIterator(BytewiseComparator());
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  iter->Seek("a");
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

TEST(BlockBuilderTest, ResetStartsAFreshBlock) {
  BlockBuilder builder(4);
  builder.Add("b", "1");
  builder.Finish();
  builder.Reset();
  builder.Add("a", "2");  // Smaller than the key of the abandoned block.
  Block block(CopyToContents(builder.Finish()));
  auto iter = block.NewIterator(BytewiseComparator());
  EXPECT_EQ((std::vector<Entry>{{"a", "2"}}), ScanIterator(iter.get()));
}

TEST(BlockTest, MalformedBlocksReportCorruption) {
  // Too short to hold the restart count.
  {
    Block block(CopyToContents("abc"));
    auto iter = block.NewIterator(BytewiseComparator());
    iter->SeekToFirst();
    EXPECT_FALSE(iter->Valid());
    EXPECT_TRUE(iter->status().IsCorruption());
  }
  // A restart count larger than the block could hold.
  {
    std::string bytes;
    PutFixed32(&bytes, 1000);
    Block block(CopyToContents(bytes));
    auto iter = block.NewIterator(BytewiseComparator());
    iter->SeekToFirst();
    EXPECT_TRUE(iter->status().IsCorruption());
  }
  // An entry whose lengths run past the end of the entry area.
  {
    std::string bytes;
    PutVarint32(&bytes, 0);    // shared
    PutVarint32(&bytes, 50);   // non_shared: more bytes than exist
    PutVarint32(&bytes, 1);    // value length
    bytes += "key";
    PutFixed32(&bytes, 0);  // restart[0]
    PutFixed32(&bytes, 1);  // num_restarts
    Block block(CopyToContents(bytes));
    auto iter = block.NewIterator(BytewiseComparator());
    iter->SeekToFirst();
    EXPECT_FALSE(iter->Valid());
    EXPECT_TRUE(iter->status().IsCorruption());
  }
  // A restart point outside the entry area.
  {
    BlockBuilder builder(1);
    builder.Add("a", "1");
    builder.Add("b", "2");
    std::string bytes(builder.Finish());
    EncodeFixed32(bytes.data() + bytes.size() - 8, 1000);  // restart[1]
    Block block(CopyToContents(bytes));
    auto iter = block.NewIterator(BytewiseComparator());
    iter->Seek("b");
    EXPECT_FALSE(iter->Valid());
    EXPECT_TRUE(iter->status().IsCorruption());
  }
}

// Random bytes dressed up with a plausible restart array: every decoding
// path is exercised against garbage, and the sanitizer builds check that
// none of them reads outside the block.
TEST(BlockTest, GarbageBlocksNeverReadOutOfBounds) {
  std::mt19937_64 rng(99);
  for (int round = 0; round < 2000; ++round) {
    const size_t entry_bytes = rng() % 64;
    std::string bytes = test::RandomBytes(&rng, entry_bytes);
    const uint32_t num_restarts = 1 + static_cast<uint32_t>(rng() % 4);
    for (uint32_t i = 0; i < num_restarts; ++i) {
      PutFixed32(&bytes, static_cast<uint32_t>(rng() % (entry_bytes + 3)));
    }
    PutFixed32(&bytes, num_restarts);
    Block block(CopyToContents(bytes));
    auto iter = block.NewIterator(BytewiseComparator());
    int steps = 0;
    for (iter->SeekToFirst(); iter->Valid() && steps < 100; iter->Next(), ++steps) {
      static_cast<void>(iter->key());
      static_cast<void>(iter->value());
    }
    iter->Seek(test::RandomBytes(&rng, rng() % 4));
    if (iter->Valid()) {
      static_cast<void>(iter->key());
    }
  }
}

// ---------------------------------------------------------------------------
// Format helpers

TEST(TableFormatTest, BlockHandleRoundTrips) {
  for (const auto& [offset, size] :
       {std::pair<uint64_t, uint64_t>{0, 0}, {1, 4096}, {uint64_t{1} << 40, 123456789},
        {~uint64_t{0}, ~uint64_t{0}}}) {
    std::string encoded;
    BlockHandle(offset, size).EncodeTo(&encoded);
    EXPECT_LE(encoded.size(), BlockHandle::kMaxEncodedLength);
    std::string_view input(encoded);
    BlockHandle decoded;
    ASSERT_OK(decoded.DecodeFrom(&input));
    EXPECT_TRUE(input.empty());
    EXPECT_EQ(offset, decoded.offset());
    EXPECT_EQ(size, decoded.size());
  }
  std::string_view truncated("\x80", 1);
  BlockHandle handle;
  EXPECT_TRUE(handle.DecodeFrom(&truncated).IsCorruption());
}

TEST(TableFormatTest, FooterRoundTripsAndRejectsForeignFiles) {
  std::string encoded;
  Footer(BlockHandle(100, 20), BlockHandle(125, 300)).EncodeTo(&encoded);
  ASSERT_EQ(Footer::kEncodedLength, encoded.size());
  Footer footer;
  ASSERT_OK(footer.DecodeFrom(encoded));
  EXPECT_EQ(100u, footer.metaindex_handle().offset());
  EXPECT_EQ(20u, footer.metaindex_handle().size());
  EXPECT_EQ(125u, footer.index_handle().offset());
  EXPECT_EQ(300u, footer.index_handle().size());

  std::string bad_magic = encoded;
  bad_magic.back() = static_cast<char>(bad_magic.back() ^ 1);
  EXPECT_TRUE(footer.DecodeFrom(bad_magic).IsCorruption());

  std::string bad_version = encoded;
  EncodeFixed32(bad_version.data() + Footer::kEncodedLength - 12, kTableFormatVersion + 1);
  EXPECT_TRUE(footer.DecodeFrom(bad_version).IsCorruption());

  EXPECT_TRUE(footer.DecodeFrom(std::string_view(encoded).substr(1)).IsCorruption());
}

// ---------------------------------------------------------------------------
// Tables

std::string IKey(std::string_view user_key, SequenceNumber sequence, ValueType type) {
  std::string result;
  AppendInternalKey(&result, ParsedInternalKey(user_key, sequence, type));
  return result;
}

class TableTest : public testing::Test {
 protected:
  // Builds a table holding `entries`, which must be in internal key order.
  void Build(const std::vector<Entry>& entries) {
    contents_.clear();
    test::StringSink sink(&contents_);
    TableBuilder builder(options_, &icmp_, &sink);
    for (const auto& [key, value] : entries) {
      builder.Add(key, value);
    }
    ASSERT_OK(builder.Finish());
    EXPECT_EQ(contents_.size(), builder.FileSize());
    EXPECT_EQ(entries.size(), builder.NumEntries());
  }

  Status Open(std::shared_ptr<Cache> block_cache = nullptr) {
    auto file = std::make_unique<test::StringFile>(&contents_);
    file_ = file.get();
    return Table::Open(options_, &icmp_, std::move(file), /*file_number=*/7, contents_.size(),
                       &table_, std::move(block_cache));
  }

  // Looks up `ikey`; returns the entry found, or "" if none was passed on.
  std::string Lookup(const std::string& ikey) {
    std::string found;
    EXPECT_OK(table_->InternalGet(ReadOptions(), ikey,
                                  [&](std::string_view key, std::string_view value) {
                                    found = std::string(key) + "=" + std::string(value);
                                  }));
    return found;
  }

  Footer ReadFooter() const {
    Footer footer;
    EXPECT_OK(footer.DecodeFrom(
        std::string_view(contents_).substr(contents_.size() - Footer::kEncodedLength)));
    return footer;
  }

  // Offset where the data blocks end and the meta blocks (filter, if any,
  // then metaindex) start: the end of the block the last index entry names.
  uint64_t DataEnd() const {
    const Footer footer = ReadFooter();
    Block index(CopyToContents(std::string_view(contents_).substr(
        footer.index_handle().offset(), footer.index_handle().size())));
    auto iter = index.NewIterator(&icmp_);
    uint64_t end = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      BlockHandle handle;
      std::string_view input = iter->value();
      EXPECT_OK(handle.DecodeFrom(&input));
      end = handle.offset() + handle.size() + kBlockTrailerSize;
    }
    return end;
  }

  // Entries (name, handle) of the metaindex block.
  std::vector<std::pair<std::string, BlockHandle>> MetaIndex() const {
    const Footer footer = ReadFooter();
    Block metaindex(CopyToContents(std::string_view(contents_).substr(
        footer.metaindex_handle().offset(), footer.metaindex_handle().size())));
    auto iter = metaindex.NewIterator(BytewiseComparator());
    std::vector<std::pair<std::string, BlockHandle>> result;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      BlockHandle handle;
      std::string_view input = iter->value();
      EXPECT_OK(handle.DecodeFrom(&input));
      result.emplace_back(std::string(iter->key()), handle);
    }
    EXPECT_OK(iter->status());
    return result;
  }

  // Random entries in internal key order: binary user keys (the empty key
  // included), several versions of some keys, tombstones, empty values and
  // a few values much larger than a block.
  std::vector<Entry> RandomEntries(std::mt19937_64* rng, int count) {
    std::vector<Entry> entries;
    SequenceNumber sequence = 1;
    entries.emplace_back(IKey("", sequence++, kTypeValue), "value of the empty key");
    while (static_cast<int>(entries.size()) < count) {
      const std::string user_key = test::RandomBytes(rng, (*rng)() % 12);
      const bool tombstone = (*rng)() % 5 == 0;
      std::string value;
      if (!tombstone) {
        const uint64_t kind = (*rng)() % 50;
        const size_t size = kind == 0 ? 20000 + (*rng)() % 20000 : kind < 10 ? 0 : (*rng)() % 100;
        value = test::RandomBytes(rng, size);
      }
      // Short random keys collide often, giving several versions of them.
      entries.emplace_back(IKey(user_key, sequence++, tombstone ? kTypeDeletion : kTypeValue),
                           std::move(value));
    }
    std::sort(entries.begin(), entries.end(), [this](const Entry& a, const Entry& b) {
      return icmp_.Compare(a.first, b.first) < 0;
    });
    return entries;
  }

  Options options_;
  const InternalKeyComparator icmp_{BytewiseComparator()};
  std::string contents_;
  test::StringFile* file_ = nullptr;
  std::shared_ptr<Table> table_;
};

TEST_F(TableTest, RoundTripsManyBlocksOfBinaryKeysEmptyLargeValuesAndTombstones) {
  options_.block_size = 1024;
  std::mt19937_64 rng(2024);
  const std::vector<Entry> entries = RandomEntries(&rng, 3000);
  Build(entries);
  ASSERT_OK(Open());

  const int reads_before_scan = file_->reads();
  auto iter = table_->NewIterator(ReadOptions());
  EXPECT_EQ(entries, ScanIterator(iter.get()));
  EXPECT_TRUE(iter->status().ok()) << iter->status().ToString();
  // One read per data block: the table really has many blocks.
  EXPECT_GT(file_->reads() - reads_before_scan, 100);

  // Tombstones come back as tombstones, in their place in the order.
  size_t tombstones = 0;
  for (const auto& [key, value] : entries) {
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(key, &parsed));
    if (parsed.type == kTypeDeletion) {
      ++tombstones;
      EXPECT_TRUE(value.empty());
    }
  }
  EXPECT_GT(tombstones, 100u);
}

TEST_F(TableTest, SeekFindsEveryKeyAndTheSuccessorOfGaps) {
  options_.block_size = 512;
  std::mt19937_64 rng(7);
  const std::vector<Entry> entries = RandomEntries(&rng, 800);
  Build(entries);
  ASSERT_OK(Open());

  auto iter = table_->NewIterator(ReadOptions());
  for (size_t i = 0; i < entries.size(); ++i) {
    iter->Seek(entries[i].first);
    ASSERT_TRUE(iter->Valid()) << i;
    ASSERT_EQ(entries[i].first, iter->key()) << i;
    EXPECT_EQ(entries[i].second, iter->value()) << i;
    // Sequence 0 is the oldest version of a user key, so this target falls
    // after every version of entry i's user key.
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(entries[i].first, &parsed));
    const std::string after = IKey(parsed.user_key, 0, kTypeDeletion);
    iter->Seek(after);
    const auto expected = std::upper_bound(
        entries.begin(), entries.end(), after,
        [this](const std::string& target, const Entry& e) {
          return icmp_.Compare(target, e.first) < 0;
        });
    if (expected == entries.end()) {
      EXPECT_FALSE(iter->Valid());
    } else {
      ASSERT_TRUE(iter->Valid());
      EXPECT_EQ(expected->first, iter->key());
    }
  }
  iter->Seek(IKey(std::string(20, '\xff'), kMaxSequenceNumber, kTypeValue));
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

TEST_F(TableTest, InternalGetReadsOneDataBlock) {
  options_.block_size = 256;
  std::mt19937_64 rng(11);
  const std::vector<Entry> entries = RandomEntries(&rng, 600);
  Build(entries);
  ASSERT_OK(Open());

  for (const auto& [key, value] : entries) {
    const int before = file_->reads();
    int calls = 0;
    ASSERT_OK(table_->InternalGet(ReadOptions(), key,
                                  [&](std::string_view found_key, std::string_view found_value) {
                                    ++calls;
                                    EXPECT_EQ(key, found_key);
                                    EXPECT_EQ(value, found_value);
                                  }));
    EXPECT_EQ(1, calls);
    EXPECT_EQ(1, file_->reads() - before);
  }

  // Past the last key no data block can hold the key, so none is read.
  const int before = file_->reads();
  bool called = false;
  ASSERT_OK(table_->InternalGet(ReadOptions(), IKey(std::string(20, '\xff'), 1, kTypeValue),
                                [&](std::string_view, std::string_view) { called = true; }));
  EXPECT_FALSE(called);
  EXPECT_EQ(0, file_->reads() - before);
}

TEST_F(TableTest, ValueLargerThanManyBlocks) {
  std::mt19937_64 rng(3);
  const std::vector<Entry> entries = {
      {IKey("a", 3, kTypeValue), "small"},
      {IKey("b", 2, kTypeValue), test::RandomBytes(&rng, 3 << 20)},
      {IKey("c", 1, kTypeDeletion), ""},
  };
  Build(entries);
  ASSERT_OK(Open());
  auto iter = table_->NewIterator(ReadOptions());
  EXPECT_EQ(entries, ScanIterator(iter.get()));
}

TEST_F(TableTest, KeysOutOfOrderFailTheBuilder) {
  for (const auto& second : {IKey("a", 5, kTypeValue), IKey("a", 4, kTypeValue)}) {
    contents_.clear();
    test::StringSink sink(&contents_);
    TableBuilder builder(options_, &icmp_, &sink);
    builder.Add(IKey("b", 1, kTypeValue), "1");
    builder.Add(second, "2");  // Sorts before the first key.
    EXPECT_TRUE(builder.status().IsInvalidArgument());
    builder.Add(IKey("c", 1, kTypeValue), "3");  // Ignored once failed.
    EXPECT_TRUE(builder.Finish().IsInvalidArgument());
    EXPECT_EQ(1u, builder.NumEntries());
  }
  // A duplicate internal key is out of order too.
  contents_.clear();
  test::StringSink sink(&contents_);
  TableBuilder builder(options_, &icmp_, &sink);
  builder.Add(IKey("a", 1, kTypeValue), "1");
  builder.Add(IKey("a", 1, kTypeValue), "1");
  EXPECT_TRUE(builder.Finish().IsInvalidArgument());
}

TEST_F(TableTest, EmptyTableIsValid) {
  Build({});
  ASSERT_OK(Open());
  auto iter = table_->NewIterator(ReadOptions());
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
  bool called = false;
  ASSERT_OK(table_->InternalGet(ReadOptions(), IKey("k", 1, kTypeValue),
                                [&](std::string_view, std::string_view) { called = true; }));
  EXPECT_FALSE(called);
}

TEST_F(TableTest, DamagedDataBlockIsReportedNotSkipped) {
  options_.block_size = 512;
  std::mt19937_64 rng(13);
  std::vector<Entry> entries;
  for (int i = 0; i < 400; ++i) {
    entries.emplace_back(IKey(test::Key(static_cast<uint64_t>(i)), 1, kTypeValue),
                         test::RandomBytes(&rng, 30));
  }
  Build(entries);
  const std::string pristine = contents_;
  const uint64_t data_end = DataEnd();

  for (const uint64_t offset : {uint64_t{0}, data_end / 3, data_end / 2, data_end - 1}) {
    contents_ = pristine;
    contents_[offset] = static_cast<char>(contents_[offset] ^ 0x40);
    ASSERT_OK(Open());  // The index is intact; the damage shows up on read.

    auto iter = table_->NewIterator(ReadOptions());
    const std::vector<Entry> scanned = ScanIterator(iter.get());
    EXPECT_TRUE(iter->status().IsCorruption()) << "offset " << offset;
    ASSERT_LT(scanned.size(), entries.size());
    // Everything returned before the error is correct: a prefix.
    EXPECT_TRUE(std::equal(scanned.begin(), scanned.end(), entries.begin()));

    // A point lookup of the first key after the returned prefix lands in
    // the damaged block.
    const std::string& victim = entries[scanned.size()].first;
    const Status s =
        table_->InternalGet(ReadOptions(), victim, [](std::string_view, std::string_view) {});
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_NE(std::string::npos, s.message().find("table 7")) << s.ToString();
  }
}

TEST_F(TableTest, DamagedIndexOrFooterFailsOpen) {
  std::vector<Entry> entries;
  for (int i = 0; i < 100; ++i) {
    entries.emplace_back(IKey(test::Key(static_cast<uint64_t>(i)), 1, kTypeValue), "v");
  }
  Build(entries);
  const std::string pristine = contents_;
  const size_t footer_start = pristine.size() - Footer::kEncodedLength;

  // Any byte of the metaindex block, the index block or the footer's magic.
  for (uint64_t offset = DataEnd(); offset < footer_start; ++offset) {
    contents_ = pristine;
    contents_[offset] = static_cast<char>(contents_[offset] ^ 0x08);
    EXPECT_TRUE(Open().IsCorruption()) << "offset " << offset;
  }
  contents_ = pristine;
  contents_.back() = static_cast<char>(contents_.back() ^ 0x08);
  EXPECT_TRUE(Open().IsCorruption());

  // Truncated tables, including one too short to hold a footer.
  for (const size_t keep :
       {pristine.size() - 1, footer_start, Footer::kEncodedLength - 1, size_t{0}}) {
    contents_ = pristine.substr(0, keep);
    EXPECT_TRUE(Open().IsCorruption()) << "kept " << keep << " bytes";
  }

  // Not a table at all.
  std::mt19937_64 rng(1);
  contents_ = test::RandomBytes(&rng, 4096);
  EXPECT_TRUE(Open().IsCorruption());
}

// ---------------------------------------------------------------------------
// Bloom filters in tables

// Even-numbered keys; the odd ones in between are the missing keys.
std::vector<Entry> EvenKeys(int count) {
  std::vector<Entry> entries;
  for (int i = 0; i < count; ++i) {
    entries.emplace_back(IKey(test::Key(static_cast<uint64_t>(2 * i)), 1, kTypeValue),
                         "v" + std::to_string(i));
  }
  return entries;
}

TEST_F(TableTest, FilterBlockIsListedInTheMetaindex) {
  options_.bloom_bits_per_key = 10;
  Build(EvenKeys(1000));
  const auto meta = MetaIndex();
  ASSERT_EQ(1u, meta.size());
  EXPECT_EQ(kFilterBlockName, meta[0].first);
  // Between the data blocks and the metaindex, 10 bits per key plus k.
  EXPECT_EQ(DataEnd(), meta[0].second.offset());
  EXPECT_EQ(1000u * 10 / 8 + 1, meta[0].second.size());
  EXPECT_EQ(ReadFooter().metaindex_handle().offset(),
            meta[0].second.offset() + meta[0].second.size() + kBlockTrailerSize);
  ASSERT_OK(Open());
  EXPECT_TRUE(table_->has_filter());

  options_.bloom_bits_per_key = 0;
  Build(EvenKeys(1000));
  EXPECT_TRUE(MetaIndex().empty());
  EXPECT_EQ(DataEnd(), ReadFooter().metaindex_handle().offset());
  ASSERT_OK(Open());
  EXPECT_FALSE(table_->has_filter());
}

TEST_F(TableTest, FilterSkipsMissingKeysWithoutReadingBlocks) {
  options_.bloom_bits_per_key = 10;
  options_.block_size = 512;
  auto stats = std::make_shared<Statistics>();
  options_.statistics = stats;
  const std::vector<Entry> entries = EvenKeys(5000);
  Build(entries);
  ASSERT_OK(Open());

  // Present keys: always found, one block each, never a false positive.
  for (const auto& [key, value] : entries) {
    ASSERT_EQ(key + "=" + value, Lookup(key));
  }
  EXPECT_EQ(5000u, stats->bloom_checks.load());
  EXPECT_EQ(0u, stats->bloom_useful.load());
  EXPECT_EQ(0u, stats->bloom_false_positives.load());
  EXPECT_EQ(5000u, stats->block_reads.load());

  // Missing keys inside the table's key range: the index alone cannot rule
  // them out, the filter can.
  stats->Reset();
  const int reads_before = file_->reads();
  for (int i = 0; i < 5000; ++i) {
    const std::string missing = IKey(test::Key(static_cast<uint64_t>(2 * i + 1)), 1, kTypeValue);
    const std::string found = Lookup(missing);
    // Without a filter hit, nothing is passed on; with one, the entry after
    // the key is (the caller checks the user key).
    EXPECT_EQ(std::string::npos, found.find(test::Key(static_cast<uint64_t>(2 * i + 1))));
  }
  const uint64_t false_positives = stats->bloom_false_positives.load();
  EXPECT_EQ(5000u, stats->bloom_checks.load());
  EXPECT_EQ(5000u, stats->bloom_useful.load() + false_positives);
  EXPECT_LT(false_positives, 100u);  // < 2%
  // Only false positives cost a block read.
  EXPECT_EQ(false_positives, stats->block_reads.load());
  EXPECT_EQ(static_cast<int>(false_positives), file_->reads() - reads_before);
}

TEST_F(TableTest, TablesWithoutAFilterAreReadNormally) {
  // Written without a filter (as every phase 1 table was) and read by a
  // reader configured to build filters: nothing is skipped or counted.
  options_.bloom_bits_per_key = 0;
  const std::vector<Entry> entries = EvenKeys(500);
  Build(entries);
  options_.bloom_bits_per_key = 10;
  auto stats = std::make_shared<Statistics>();
  options_.statistics = stats;
  ASSERT_OK(Open());
  EXPECT_FALSE(table_->has_filter());
  for (const auto& [key, value] : entries) {
    ASSERT_EQ(key + "=" + value, Lookup(key));
  }
  EXPECT_EQ("", Lookup(IKey(test::Key(100000), 1, kTypeValue)));
  EXPECT_EQ(0u, stats->bloom_checks.load());
  auto iter = table_->NewIterator(ReadOptions());
  EXPECT_EQ(entries, ScanIterator(iter.get()));
}

TEST_F(TableTest, FilterCoversEveryVersionAndTombstone) {
  options_.bloom_bits_per_key = 10;
  options_.block_size = 256;
  std::mt19937_64 rng(5);
  const std::vector<Entry> entries = RandomEntries(&rng, 2000);
  Build(entries);
  ASSERT_OK(Open());
  ASSERT_TRUE(table_->has_filter());
  // Every stored internal key (all versions, tombstones, the empty key) is
  // found: the filter never hides one.
  for (const auto& [key, value] : entries) {
    ASSERT_EQ(key + "=" + value, Lookup(key));
  }
  // A lookup at a newer sequence than any stored still reaches the newest
  // version of its user key.
  for (const auto& [key, value] : entries) {
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(key, &parsed));
    const std::string found = Lookup(IKey(parsed.user_key, kMaxSequenceNumber, kTypeValue));
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(0, found.compare(0, parsed.user_key.size(), parsed.user_key));
  }
}

TEST_F(TableTest, KeysShorterThanATagFailABuilderWithAFilter) {
  options_.bloom_bits_per_key = 10;
  contents_.clear();
  test::StringSink sink(&contents_);
  TableBuilder builder(options_, BytewiseComparator(), &sink);
  builder.Add("short", "v");
  EXPECT_TRUE(builder.Finish().IsInvalidArgument());
}

// ---------------------------------------------------------------------------
// Block cache in tables

TEST_F(TableTest, PointLookupsFillTheBlockCacheAndScansBypassIt) {
  options_.block_size = 512;
  auto stats = std::make_shared<Statistics>();
  options_.statistics = stats;
  const std::vector<Entry> entries = EvenKeys(2000);
  Build(entries);
  auto cache = std::make_shared<Cache>(1 << 20);
  ASSERT_OK(Open(cache));

  // A scan reads every block from the file and leaves the cache empty.
  {
    auto iter = table_->NewIterator(ReadOptions());
    EXPECT_EQ(entries, ScanIterator(iter.get()));
  }
  EXPECT_EQ(0u, cache->TotalCharge());
  EXPECT_EQ(0u, stats->block_cache_hits.load() + stats->block_cache_misses.load());
  const uint64_t num_blocks = stats->block_reads.load();
  EXPECT_GT(num_blocks, 20u);

  stats->Reset();
  for (int pass = 0; pass < 3; ++pass) {
    const int reads_before = file_->reads();
    for (const auto& [key, value] : entries) {
      ASSERT_EQ(key + "=" + value, Lookup(key));
    }
    // The first pass reads each block once; later passes read nothing.
    EXPECT_EQ(pass == 0 ? static_cast<int>(num_blocks) : 0, file_->reads() - reads_before);
  }
  EXPECT_EQ(num_blocks, stats->block_reads.load());
  EXPECT_EQ(num_blocks, stats->block_cache_misses.load());
  EXPECT_EQ(3 * entries.size() - num_blocks, stats->block_cache_hits.load());
  EXPECT_GT(cache->TotalCharge(), 0u);
}

TEST_F(TableTest, BlocksStayValidWhenTheCacheEvictsThem) {
  options_.block_size = 512;
  std::mt19937_64 rng(17);
  const std::vector<Entry> entries = RandomEntries(&rng, 1500);
  Build(entries);
  // Room for barely one block: every lookup evicts the previous block, and
  // the sanitizer builds would catch a read of a freed one.
  auto cache = std::make_shared<Cache>(600, 0);
  ASSERT_OK(Open(cache));
  for (const auto& [key, value] : entries) {
    ASSERT_EQ(key + "=" + value, Lookup(key));
  }
  EXPECT_LE(cache->TotalCharge(), 600u);
  // The table can go before the cache, and the cache before the table.
  table_.reset();
  ASSERT_OK(Open(cache));
  EXPECT_EQ(entries[0].first + "=" + entries[0].second, Lookup(entries[0].first));
  cache.reset();
  EXPECT_EQ(entries[1].first + "=" + entries[1].second, Lookup(entries[1].first));
}

TEST_F(TableTest, ConcurrentLookupsShareTheCache) {
  options_.block_size = 256;
  const std::vector<Entry> entries = EvenKeys(3000);
  Build(entries);
  auto cache = std::make_shared<Cache>(16 * 1024, 2);  // Smaller than the table.
  ASSERT_OK(Open(cache));
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t));
      for (int i = 0; i < 5000; ++i) {
        const auto& [key, value] = entries[rng() % entries.size()];
        std::string found;
        ASSERT_OK(table_->InternalGet(ReadOptions(), key,
                                      [&](std::string_view k, std::string_view v) {
                                        found = std::string(k) + "=" + std::string(v);
                                      }));
        ASSERT_EQ(key + "=" + value, found);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_LE(cache->TotalCharge(), 16u * 1024);
}

// ---------------------------------------------------------------------------
// Filters across the tables of a database

class TableFilterDBTest : public test::DBTestBase {
 protected:
  TableFilterDBTest() {
    options_.statistics = stats_;
    options_.disable_auto_compactions = true;  // Keep every level-0 table.
    options_.block_cache_capacity = 0;         // Count every block read.
    options_.block_size = 1024;
    UseNoSyncEnv();
  }

  // `tables` level-0 tables that all span the whole key range: table t
  // holds the keys i with i % tables == t, i in [0, keys).
  void FillInterleavedTables(int tables, int keys) {
    for (int t = 0; t < tables; ++t) {
      for (int i = t; i < keys; i += tables) {
        ASSERT_OK(Put(test::Key(static_cast<uint64_t>(i)), "v" + std::to_string(i)));
      }
      ASSERT_OK(FlushAndSettle());
    }
    ASSERT_EQ(tables, FilesAtLevel(0));
  }

  // Looks up `count` keys that fall between stored keys, so every table's
  // key range and index admit them (the first few keys are skipped: they
  // sort before the smallest key of some tables).
  void LookUpMissingKeys(int count) {
    for (int i = 10; i < count + 10; ++i) {
      ASSERT_EQ("NOT_FOUND", Get(test::Key(static_cast<uint64_t>(i)) + "-missing"));
    }
  }

  std::shared_ptr<Statistics> stats_ = std::make_shared<Statistics>();
};

TEST_F(TableFilterDBTest, MissingKeyLookupsRarelyTouchADataBlock) {
  ASSERT_OK(Reopen());
  FillInterleavedTables(5, 20000);
  stats_->Reset();
  LookUpMissingKeys(10000);

  // Every lookup asks every table's filter...
  const uint64_t probes = stats_->bloom_checks.load();
  EXPECT_EQ(5u * 10000, probes);
  EXPECT_EQ(probes, stats_->bloom_useful.load() + stats_->bloom_false_positives.load());
  // ...and only the filters' false positives read a data block.
  const uint64_t block_reads = stats_->block_reads.load();
  EXPECT_EQ(stats_->bloom_false_positives.load(), block_reads);
  const double touched = static_cast<double>(block_reads) / static_cast<double>(probes);
  EXPECT_LE(touched, 0.02);
  std::printf("[ filter   ] missing-key probes %llu, data blocks read %llu (%.4f)\n",
              static_cast<unsigned long long>(probes),
              static_cast<unsigned long long>(block_reads), touched);

  // Present keys still read exactly one block in the one table holding them.
  stats_->Reset();
  for (int i = 0; i < 20000; i += 7) {
    ASSERT_EQ("v" + std::to_string(i), Get(test::Key(static_cast<uint64_t>(i))));
  }
  EXPECT_EQ((20000u + 6) / 7, stats_->block_reads.load() - stats_->bloom_false_positives.load());
}

TEST_F(TableFilterDBTest, WithoutFiltersEveryProbeReadsABlock) {
  options_.bloom_bits_per_key = 0;
  ASSERT_OK(Reopen());
  FillInterleavedTables(5, 20000);
  stats_->Reset();
  LookUpMissingKeys(2000);
  EXPECT_EQ(0u, stats_->bloom_checks.load());
  EXPECT_EQ(5u * 2000, stats_->block_reads.load());
}

TEST_F(TableFilterDBTest, TablesWithAndWithoutFiltersMix) {
  // Tables written without filters stay readable after filters are enabled;
  // only the newer tables are filtered.
  options_.bloom_bits_per_key = 0;
  ASSERT_OK(Reopen());
  FillInterleavedTables(2, 4000);
  options_.bloom_bits_per_key = 10;
  ASSERT_OK(Reopen());
  for (int i = 4000; i < 8000; ++i) {
    ASSERT_OK(Put(test::Key(static_cast<uint64_t>(i)), "v" + std::to_string(i)));
  }
  ASSERT_OK(FlushAndSettle());
  // Overwrite part of the old range in a filtered table.
  for (int i = 0; i < 4000; i += 3) {
    ASSERT_OK(Put(test::Key(static_cast<uint64_t>(i)), "new" + std::to_string(i)));
  }
  ASSERT_OK(FlushAndSettle());

  stats_->Reset();
  for (int i = 0; i < 8000; ++i) {
    const std::string expected =
        (i < 4000 && i % 3 == 0 ? "new" : "v") + std::to_string(i);
    ASSERT_EQ(expected, Get(test::Key(static_cast<uint64_t>(i)))) << i;
  }
  EXPECT_GT(stats_->bloom_checks.load(), 0u);
  ASSERT_OK(Reopen());
  EXPECT_EQ(8000u, Scan().size());
}

}  // namespace
}  // namespace kvlog
