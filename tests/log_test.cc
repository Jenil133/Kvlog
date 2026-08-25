#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "db/log_format.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace kvlog::log {
namespace {

struct ReadResult {
  std::vector<std::string> records;
  Reader::State state = Reader::State::kReading;
  Status status;
  uint64_t valid_end = 0;
};

ReadResult ReadAll(const std::string& contents) {
  ReadResult result;
  Reader reader(std::make_unique<test::StringSource>(&contents));
  std::string record;
  while (reader.ReadRecord(&record)) {
    result.records.push_back(record);
  }
  result.state = reader.state();
  result.status = reader.status();
  result.valid_end = reader.valid_end();
  return result;
}

// Builds a 13-byte record header with correct checksums for the given
// fields, so tests can write records the Writer never would.
std::string MakeHeader(std::string_view payload, uint32_t length, uint8_t type) {
  char header[kHeaderSize];
  EncodeFixed32(header + 4, crc32c::Mask(crc32c::Value(payload.data(), payload.size())));
  EncodeFixed32(header + 8, length);
  header[12] = static_cast<char>(type);
  EncodeFixed32(header, crc32c::Mask(crc32c::Value(header + 4, kHeaderSize - 4)));
  return std::string(header, kHeaderSize);
}

class LogTest : public testing::Test {
 protected:
  LogTest() : writer_(std::make_unique<test::StringSink>(&contents_)) {}

  void Write(const std::string& payload) {
    ASSERT_OK(writer_.AddRecord(payload));
    written_.push_back(payload);
    ends_.push_back(contents_.size());
  }

  // Offset of the first byte of record `i`.
  uint64_t Start(size_t i) const { return i == 0 ? 0 : ends_[i - 1]; }

  std::vector<std::string> FirstRecords(size_t n) const {
    return std::vector<std::string>(written_.begin(),
                                    written_.begin() + static_cast<std::ptrdiff_t>(n));
  }

  std::string contents_;
  Writer writer_;
  std::vector<std::string> written_;
  std::vector<uint64_t> ends_;  // File offset just past each record.
};

TEST_F(LogTest, EmptyLogEndsCleanly) {
  const ReadResult result = ReadAll(contents_);
  EXPECT_TRUE(result.records.empty());
  EXPECT_EQ(Reader::State::kEof, result.state);
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(0u, result.valid_end);
}

TEST_F(LogTest, RecordLayoutMatchesTheDocumentedFormat) {
  Write("hello");
  ASSERT_EQ(kHeaderSize + 5, contents_.size());
  EXPECT_EQ(5u, DecodeFixed32(contents_.data() + 8));
  EXPECT_EQ(kFullType, static_cast<uint8_t>(contents_[12]));
  EXPECT_EQ(crc32c::Value("hello", 5), crc32c::Unmask(DecodeFixed32(contents_.data() + 4)));
  EXPECT_EQ(crc32c::Value(contents_.data() + 4, kHeaderSize - 4),
            crc32c::Unmask(DecodeFixed32(contents_.data())));
  EXPECT_EQ("hello", contents_.substr(kHeaderSize));
  EXPECT_EQ(contents_.size(), writer_.size());
}

TEST_F(LogTest, RoundTripsRecordsOfManySizes) {
  std::mt19937_64 rng(301);
  // Sizes around the header size and the reader's 32 KiB buffer, plus a few
  // large records that span many buffer refills.
  const size_t sizes[] = {0,         1,         12,        13,     14,      100,
                          32 * 1024 - kHeaderSize, 32 * 1024, 32 * 1024 + 1, 100000, 1 << 20};
  for (const size_t size : sizes) {
    Write(test::RandomBytes(&rng, size));
  }
  for (int i = 0; i < 1000; ++i) {
    Write(test::RandomBytes(&rng, rng() % 64));
  }

  const ReadResult result = ReadAll(contents_);
  EXPECT_EQ(written_, result.records);
  EXPECT_EQ(Reader::State::kEof, result.state);
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(contents_.size(), result.valid_end);
  EXPECT_EQ(contents_.size(), writer_.size());
}

TEST_F(LogTest, PiecesAreConcatenatedIntoOnePayload) {
  ASSERT_OK(writer_.AddRecord({"ab", "", "cde", std::string_view("\0f", 2)}));
  const ReadResult result = ReadAll(contents_);
  ASSERT_EQ(1u, result.records.size());
  EXPECT_EQ(std::string("abcde\0f", 7), result.records[0]);
}

// A crash can cut the log off anywhere. Whatever the cut, the reader must
// return exactly the records that were completely written before it, and
// classify the end as clean (cut on a record boundary) or torn (cut inside
// a record), never as corruption.
TEST_F(LogTest, TruncationAtEveryOffsetKeepsExactlyTheCompleteRecords) {
  std::mt19937_64 rng(17);
  Write("first");
  Write("");
  Write(test::RandomBytes(&rng, 300));
  Write("x");
  Write(test::RandomBytes(&rng, 40000));  // Crosses a reader buffer boundary.
  Write("last record of the log");

  size_t complete = 0;  // Records entirely below the cut.
  for (uint64_t cut = 0; cut <= contents_.size(); ++cut) {
    while (complete < ends_.size() && ends_[complete] <= cut) {
      ++complete;
    }
    const ReadResult result = ReadAll(contents_.substr(0, cut));
    ASSERT_EQ(FirstRecords(complete), result.records) << "cut at " << cut;
    const bool on_boundary = cut == Start(complete);
    EXPECT_EQ(on_boundary ? Reader::State::kEof : Reader::State::kTornTail, result.state)
        << "cut at " << cut;
    EXPECT_TRUE(result.status.ok()) << "cut at " << cut;
    EXPECT_EQ(Start(complete), result.valid_end) << "cut at " << cut;
  }
}

// Damage anywhere in a record that has more records after it cannot be the
// result of a crash; it must be reported as corruption, with every record
// before the damaged one still returned.
TEST_F(LogTest, DamageBeforeTheEndIsReportedAsCorruption) {
  std::mt19937_64 rng(5);
  Write("before");
  Write(test::RandomBytes(&rng, 64));
  Write("after");

  const uint64_t start = Start(1);
  for (uint64_t offset = start; offset < ends_[1]; ++offset) {
    std::string damaged = contents_;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0x20);
    const ReadResult result = ReadAll(damaged);
    ASSERT_EQ(FirstRecords(1), result.records) << "byte " << offset;
    EXPECT_EQ(Reader::State::kCorruption, result.state) << "byte " << offset;
    EXPECT_TRUE(result.status.IsCorruption()) << "byte " << offset;
    EXPECT_EQ(start, result.valid_end) << "byte " << offset;
  }
}

// A complete final record whose bytes changed is not what an interrupted
// append leaves (that is a short file or zeros). It may have been an
// acknowledged synced write, or a MANIFEST edit already acted on, so it is
// reported as corruption rather than dropped as a torn tail.
TEST_F(LogTest, DamagedFinalPayloadIsCorruption) {
  Write("one");
  Write("two");
  Write("the final record");
  for (uint64_t offset = Start(2) + kHeaderSize; offset < contents_.size(); ++offset) {
    std::string damaged = contents_;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0x01);
    const ReadResult result = ReadAll(damaged);
    EXPECT_EQ(FirstRecords(2), result.records) << "byte " << offset;
    EXPECT_EQ(Reader::State::kCorruption, result.state) << "byte " << offset;
    EXPECT_TRUE(result.status.IsCorruption());
  }
}

// MANIFEST records end in a fixed64 tag whose high bytes are zero. A bit
// flipped just before those zeros must not pass for a zero-filled tail.
TEST_F(LogTest, DamagedFinalRecordEndingInAFewZerosIsCorruption) {
  Write("one");
  Write(std::string("tag") + std::string(6, '\0'));
  const uint64_t flip = contents_.size() - 7;  // The 'g', just before the zeros.
  ASSERT_NE(0u, flip % 512);
  std::string damaged = contents_;
  damaged[flip] = static_cast<char>(damaged[flip] ^ 0x04);
  const ReadResult result = ReadAll(damaged);
  EXPECT_EQ(FirstRecords(1), result.records);
  EXPECT_EQ(Reader::State::kCorruption, result.state);
}

// The known limit of the zero-fill rule, pinned so that it stays documented
// (docs/DESIGN.md, "How a log ends"): when a damaged final record's payload
// ends in zeros that start on a 512-byte boundary, the damage is
// indistinguishable from zero fill after power loss and is read as a torn
// tail. One byte later, the same damage is corruption. MANIFEST edits are not
// exposed to this because they always end in a non-zero marker.
TEST_F(LogTest, DamagedFinalRecordWithZerosFromASectorBoundaryIsATornTail) {
  const std::string last = std::string("tag") + std::string(6, '\0');
  for (const uint64_t shift : {uint64_t{0}, uint64_t{1}}) {
    // Sized so that the final record's zeros start at offset 512 + shift.
    const std::string first(512 + shift - 2 * kHeaderSize - 3, 'x');
    std::string contents;
    {
      Writer writer(std::make_unique<test::StringSink>(&contents));
      ASSERT_OK(writer.AddRecord(first));
      ASSERT_OK(writer.AddRecord(last));
    }
    ASSERT_EQ(512 + shift + 6, contents.size());
    const uint64_t flip = 512 + shift - 1;  // The 'g', just before the zeros.
    contents[flip] = static_cast<char>(contents[flip] ^ 0x04);
    const ReadResult result = ReadAll(contents);
    EXPECT_EQ(std::vector<std::string>{first}, result.records) << "shift " << shift;
    EXPECT_EQ(shift == 0 ? Reader::State::kTornTail : Reader::State::kCorruption, result.state)
        << "shift " << shift;
  }
}

// After power loss, zero fill starts where the data that reached the disk
// ends, which is usually a block boundary inside a record, not a record
// boundary. Zeros from inside a header or a payload to the end of the file
// are a torn tail, also when they run past where the record would have
// ended.
TEST_F(LogTest, ZeroFillStartingInsideARecordIsATornTail) {
  std::mt19937_64 rng(23);
  for (int i = 0; i < 3; ++i) {
    Write(test::RandomBytes(&rng, 3000));
  }
  const std::vector<uint64_t> zero_starts = {
      4096,                     // A block boundary inside record 1's payload.
      Start(1) + 5,             // Inside record 1's header.
      Start(1) + kHeaderSize,   // The first byte of record 1's payload.
      Start(2) + 1,             // Inside record 2's header.
      8192,                     // A block boundary inside record 2's payload.
  };
  for (const uint64_t zero_start : zero_starts) {
    for (const size_t extra : {size_t{0}, size_t{5000}}) {
      std::string damaged = contents_.substr(0, zero_start);
      damaged.resize(contents_.size() + extra, '\0');
      const ReadResult result = ReadAll(damaged);
      size_t complete = 0;
      while (complete < ends_.size() && ends_[complete] <= zero_start) {
        ++complete;
      }
      EXPECT_EQ(FirstRecords(complete), result.records) << "zeros from " << zero_start;
      EXPECT_EQ(Reader::State::kTornTail, result.state) << "zeros from " << zero_start;
      EXPECT_TRUE(result.status.ok());
    }
  }
}

// A complete header that fails its checksum cannot come from a cut-off
// append, so it is corruption even when it belongs to the last record.
TEST_F(LogTest, DamagedFinalHeaderIsCorruption) {
  Write("one");
  Write("final");
  for (uint64_t offset = Start(1); offset < Start(1) + kHeaderSize; ++offset) {
    std::string damaged = contents_;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0x80);
    const ReadResult result = ReadAll(damaged);
    EXPECT_EQ(FirstRecords(1), result.records) << "byte " << offset;
    EXPECT_EQ(Reader::State::kCorruption, result.state) << "byte " << offset;
  }
}

// Some file systems extend a file before the appended data is written; after
// a crash the missing tail reads back as zeros.
TEST_F(LogTest, ZeroFilledTailIsATornTail) {
  Write("alpha");
  Write("beta");
  for (const size_t zeros :
       {size_t{1}, kHeaderSize - 1, kHeaderSize, size_t{4096}, size_t{70000}}) {
    const ReadResult result = ReadAll(contents_ + std::string(zeros, '\0'));
    EXPECT_EQ(written_, result.records) << zeros << " zero bytes";
    EXPECT_EQ(Reader::State::kTornTail, result.state) << zeros << " zero bytes";
    EXPECT_EQ(contents_.size(), result.valid_end);
  }
}

TEST_F(LogTest, ZerosFollowedByMoreDataAreCorruption) {
  Write("alpha");
  std::string damaged = contents_ + std::string(64, '\0');
  damaged += MakeHeader("tail", 4, kFullType) + "tail";
  const ReadResult result = ReadAll(damaged);
  EXPECT_EQ(written_, result.records);
  EXPECT_EQ(Reader::State::kCorruption, result.state);
  EXPECT_TRUE(result.status.IsCorruption());
}

TEST_F(LogTest, UnknownRecordTypeIsCorruption) {
  Write("good");
  for (const uint8_t type : {uint8_t{kZeroType}, uint8_t{kMaxRecordType + 1}, uint8_t{0xff}}) {
    // The header checksum is valid, so only the type can be blamed.
    const std::string damaged = contents_ + MakeHeader("bad", 3, type) + "bad";
    const ReadResult result = ReadAll(damaged);
    EXPECT_EQ(written_, result.records) << "type " << int{type};
    EXPECT_EQ(Reader::State::kCorruption, result.state) << "type " << int{type};
  }
}

// A final header promising far more payload than the file holds is a torn
// write. The reader must not try to allocate the promised size up front.
TEST_F(LogTest, HugeLengthAtTheEndIsATornTail) {
  Write("good");
  const std::string damaged = contents_ + MakeHeader("", 0xffffffffu, kFullType) + "short";
  const ReadResult result = ReadAll(damaged);
  EXPECT_EQ(written_, result.records);
  EXPECT_EQ(Reader::State::kTornTail, result.state);
}

TEST_F(LogTest, ReadErrorsAreReportedAsIOErrors) {
  Write("first");
  Write(std::string(50000, 'x'));
  Reader reader([this] {
    auto source = std::make_unique<test::StringSource>(&contents_);
    // The reader fetches 32 KiB at a time: the first fetch succeeds, the
    // one that continues the second record's payload fails.
    source->FailAfter(32 * 1024);
    return source;
  }());
  std::string record;
  ASSERT_TRUE(reader.ReadRecord(&record));
  EXPECT_EQ("first", record);
  EXPECT_FALSE(reader.ReadRecord(&record));
  EXPECT_EQ(Reader::State::kIOError, reader.state());
  EXPECT_TRUE(reader.status().IsIOError());
  EXPECT_EQ(Start(1), reader.valid_end());
}

TEST_F(LogTest, ReaderStaysStoppedOnceDone) {
  Write("only");
  std::string damaged = contents_ + "garbage that is not a record";
  Reader reader(std::make_unique<test::StringSource>(&damaged));
  std::string record;
  ASSERT_TRUE(reader.ReadRecord(&record));
  EXPECT_FALSE(reader.ReadRecord(&record));
  const Reader::State state = reader.state();
  EXPECT_FALSE(reader.ReadRecord(&record));
  EXPECT_TRUE(record.empty());
  EXPECT_EQ(state, reader.state());
}

TEST(LogWriterTest, AppendFailuresAreReturnedAndNotCounted) {
  std::string contents;
  auto sink = std::make_unique<test::StringSink>(&contents);
  test::StringSink* const raw_sink = sink.get();
  Writer writer(std::move(sink));
  ASSERT_OK(writer.AddRecord("ok"));
  const uint64_t size = writer.size();
  raw_sink->FailAppends();
  EXPECT_TRUE(writer.AddRecord("fails").IsIOError());
  EXPECT_EQ(size, writer.size());
}

}  // namespace
}  // namespace kvlog::log
