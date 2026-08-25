#include "kvlog/write_batch.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "test_util.h"
#include "util/coding.h"

namespace kvlog {
namespace {

// Applies the batch to a fresh memtable starting at the batch's sequence and
// renders the result in memtable (internal key) order.
std::string PrintContents(const WriteBatch& batch) {
  const InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp);
  std::string state;
  Status s = WriteBatchInternal::InsertInto(batch, WriteBatchInternal::Sequence(batch), &mem);
  auto iter = mem.NewIterator();
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey ikey;
    EXPECT_TRUE(ParseInternalKey(iter->key(), &ikey));
    if (ikey.type == kTypeValue) {
      state += "Put(" + std::string(ikey.user_key) + ", " + std::string(iter->value()) + ")";
    } else {
      state += "Delete(" + std::string(ikey.user_key) + ")";
    }
    state += "@" + std::to_string(ikey.sequence);
    ++count;
  }
  if (!s.ok()) {
    state += "ParseError()";
  } else if (count != static_cast<int>(WriteBatchInternal::Count(batch))) {
    state += "CountMismatch()";
  }
  return state;
}

TEST(WriteBatchTest, Empty) {
  WriteBatch batch;
  EXPECT_EQ("", PrintContents(batch));
  EXPECT_EQ(0u, batch.Count());
  EXPECT_EQ(WriteBatchInternal::kHeaderSize, batch.ApproximateSize());
}

TEST(WriteBatchTest, MultipleOperationsGetConsecutiveSequences) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  batch.Put("baz", "boo");
  WriteBatchInternal::SetSequence(&batch, 100);
  EXPECT_EQ(100u, WriteBatchInternal::Sequence(batch));
  EXPECT_EQ(3u, batch.Count());
  EXPECT_EQ("Put(baz, boo)@102Delete(box)@101Put(foo, bar)@100", PrintContents(batch));
}

TEST(WriteBatchTest, InsertIntoUsesTheGivenSequence) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  const InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp);
  ASSERT_OK(WriteBatchInternal::InsertInto(batch, 500, &mem));
  std::string value;
  Status s;
  ASSERT_TRUE(mem.Get(LookupKey("b", 501), &value, &s));
  EXPECT_EQ("2", value);
  EXPECT_FALSE(mem.Get(LookupKey("b", 500), &value, &s));
}

TEST(WriteBatchTest, AppendConcatenatesOperations) {
  WriteBatch b1;
  WriteBatch b2;
  WriteBatchInternal::SetSequence(&b1, 200);
  WriteBatchInternal::SetSequence(&b2, 300);
  b1.Append(b2);
  EXPECT_EQ("", PrintContents(b1));
  b2.Put("a", "va");
  b1.Append(b2);
  EXPECT_EQ("Put(a, va)@200", PrintContents(b1));
  b2.Clear();
  b2.Put("b", "vb");
  b1.Append(b2);
  EXPECT_EQ("Put(a, va)@200Put(b, vb)@201", PrintContents(b1));
  b2.Delete("foo");
  b1.Append(b2);
  EXPECT_EQ("Put(a, va)@200Put(b, vb)@202Put(b, vb)@201Delete(foo)@203", PrintContents(b1));
  EXPECT_EQ(4u, b1.Count());
}

TEST(WriteBatchTest, ApproximateSizeGrows) {
  WriteBatch batch;
  const size_t empty_size = batch.ApproximateSize();
  batch.Put("foo", "bar");
  const size_t one_key_size = batch.ApproximateSize();
  EXPECT_LT(empty_size, one_key_size);
  batch.Delete("box");
  EXPECT_LT(one_key_size, batch.ApproximateSize());
}

TEST(WriteBatchTest, IterateReportsOperationsInOrder) {
  class Recorder final : public WriteBatch::Handler {
   public:
    void Put(std::string_view key, std::string_view value) override {
      log += "P:" + std::string(key) + "=" + std::string(value) + ";";
    }
    void Delete(std::string_view key) override { log += "D:" + std::string(key) + ";"; }
    std::string log;
  };
  WriteBatch batch;
  batch.Put("k1", "v1");
  batch.Delete("k2");
  batch.Put("", "");
  Recorder recorder;
  ASSERT_OK(batch.Iterate(&recorder));
  EXPECT_EQ("P:k1=v1;D:k2;P:=;", recorder.log);
}

TEST(WriteBatchTest, MalformedBatchesAreReportedAsCorruption) {
  WriteBatch good;
  good.Put("foo", "bar");
  good.Delete("box");
  const std::string contents(WriteBatchInternal::Contents(good));

  // Every truncation of the record area is detected.
  for (size_t len = WriteBatchInternal::kHeaderSize + 1; len < contents.size(); ++len) {
    WriteBatch truncated;
    WriteBatchInternal::SetContents(&truncated, std::string_view(contents).substr(0, len));
    EXPECT_TRUE(WriteBatchInternal::Validate(truncated).IsCorruption()) << len;
  }

  // A count that disagrees with the records.
  WriteBatch miscounted = good;
  WriteBatchInternal::SetCount(&miscounted, 3);
  EXPECT_TRUE(WriteBatchInternal::Validate(miscounted).IsCorruption());

  // An unknown record tag.
  std::string bad_tag = contents;
  bad_tag[WriteBatchInternal::kHeaderSize] = 0x7f;
  WriteBatch tagged;
  WriteBatchInternal::SetContents(&tagged, bad_tag);
  EXPECT_TRUE(WriteBatchInternal::Validate(tagged).IsCorruption());
  EXPECT_EQ("ParseError()", PrintContents(tagged));

  EXPECT_OK(WriteBatchInternal::Validate(good));
}

}  // namespace
}  // namespace kvlog
