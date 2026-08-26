#include "db/version_set.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "fault_env.h"
#include "table/merging_iterator.h"
#include "test_util.h"
#include "util/coding.h"
#include "util/env_util.h"

namespace kvlog {
namespace {

FileMetaData MakeMeta(uint64_t number, std::string_view smallest, std::string_view largest,
                      uint64_t size = 1000) {
  FileMetaData meta;
  meta.number = number;
  meta.file_size = size;
  meta.smallest = InternalKey(smallest, 100, kTypeValue);
  meta.largest = InternalKey(largest, 1, kTypeValue);
  return meta;
}

// ---------------------------------------------------------------------------
// VersionEdit encoding

TEST(VersionEditTest, EncodingRoundTripsEveryField) {
  VersionEdit edit;
  edit.SetComparatorName("kvlog.BytewiseComparator");
  edit.SetLogNumber(17);
  edit.SetNextFile(99);
  edit.SetLastSequence(uint64_t{1} << 50);
  edit.RemoveFile(3, 40);
  edit.RemoveFile(0, 41);
  edit.AddFile(0, MakeMeta(50, std::string("\x00\xff", 2), "zz", 4096));
  edit.AddFile(6, MakeMeta(51, "", "", 0));

  std::string encoded;
  edit.EncodeTo(&encoded);
  VersionEdit decoded;
  ASSERT_OK(decoded.DecodeFrom(encoded));
  std::string reencoded;
  decoded.EncodeTo(&reencoded);
  EXPECT_EQ(encoded, reencoded);
  EXPECT_EQ(edit.DebugString(), decoded.DebugString());
}

TEST(VersionEditTest, EmptyEditRoundTrips) {
  VersionEdit edit;
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_EQ(1u, encoded.size());  // Just the end marker.
  VersionEdit decoded;
  ASSERT_OK(decoded.DecodeFrom(encoded));
  EXPECT_TRUE(decoded.DecodeFrom("").IsCorruption());
}

// Fields often end in zero bytes (a varint of 0, the high bytes of a key's
// fixed64 tag). An encoded edit never does, so a damaged final MANIFEST
// record can never pass for zero fill left by power loss.
TEST(VersionEditTest, EncodedEditsNeverEndInAZeroByte) {
  std::vector<VersionEdit> edits(3);
  edits[0].SetComparatorName("kvlog.BytewiseComparator");  // What a new database writes.
  edits[0].SetLogNumber(0);
  edits[0].SetNextFile(2);
  edits[0].SetLastSequence(0);
  edits[1].AddFile(0, MakeMeta(5, "a", "b"));  // Ends in a key's fixed64 tag.
  edits[2].RemoveFile(1, 0);
  for (const VersionEdit& edit : edits) {
    std::string encoded;
    edit.EncodeTo(&encoded);
    ASSERT_FALSE(encoded.empty());
    EXPECT_EQ('\x7f', encoded.back()) << edit.DebugString();
    // Without the marker, the edit would end in a zero byte.
    EXPECT_EQ('\0', encoded[encoded.size() - 2]) << edit.DebugString();
  }
}

TEST(VersionEditTest, MalformedEditsAreCorruption) {
  VersionEdit edit;
  edit.SetLogNumber(5);
  edit.AddFile(1, MakeMeta(7, "a", "b"));
  std::string valid;
  edit.EncodeTo(&valid);

  VersionEdit decoded;
  // Every proper prefix cuts a field short or lacks the end marker.
  for (size_t cut = 0; cut < valid.size(); ++cut) {
    const Status s = decoded.DecodeFrom(std::string_view(valid).substr(0, cut));
    EXPECT_TRUE(s.IsCorruption()) << "cut " << cut;
  }
  EXPECT_TRUE(decoded.DecodeFrom(valid + valid).IsCorruption());  // Data after the end.

  std::string unknown_tag;
  PutVarint32(&unknown_tag, 99);
  EXPECT_TRUE(decoded.DecodeFrom(unknown_tag).IsCorruption());

  std::string bad_level;
  PutVarint32(&bad_level, 6);  // kDeletedFile
  PutVarint32(&bad_level, kNumLevels);
  PutVarint64(&bad_level, 1);
  EXPECT_TRUE(decoded.DecodeFrom(bad_level).IsCorruption());
}

// ---------------------------------------------------------------------------
// VersionSet

class VersionSetTest : public testing::Test {
 protected:
  VersionSetTest() : dbname_(dir_.Sub("db")) {
    options_.env = env_;
    EXPECT_OK(env_->CreateDir(dbname_));
  }

  void SetUp() override {
    WriteInitialManifest(BytewiseComparator()->Name());
    ASSERT_OK(Reopen());
  }

  // What DB::Open writes for a new database.
  void WriteInitialManifest(std::string_view comparator_name) {
    VersionEdit edit;
    edit.SetComparatorName(comparator_name);
    edit.SetLogNumber(0);
    edit.SetNextFile(2);
    edit.SetLastSequence(0);
    std::string record;
    edit.EncodeTo(&record);
    WriteManifest(1, {record});
  }

  void WriteManifest(uint64_t number, const std::vector<std::string>& records) {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env_->NewWritableFile(DescriptorFileName(dbname_, number), &file));
    log::Writer writer(std::move(file));
    for (const std::string& record : records) {
      ASSERT_OK(writer.AddRecord(record));
    }
    ASSERT_OK(writer.Close());
    ASSERT_OK(SetCurrentFile(env_, dbname_, number));
  }

  // Replaces the version set with a fresh one recovered from disk.
  Status Reopen() {
    vset_.reset();
    table_cache_ = std::make_unique<TableCache>(dbname_, options_, &icmp_, 100);
    vset_ = std::make_unique<VersionSet>(dbname_, &options_, table_cache_.get(), &icmp_);
    return vset_->Recover();
  }

  Status Apply(VersionEdit* edit) {
    std::unique_lock<std::mutex> lock(mu_);
    return vset_->LogAndApply(edit, &lock);
  }

  // Per level, the (number, smallest, largest) of each file.
  std::vector<std::vector<std::tuple<uint64_t, std::string, std::string>>> Files() const {
    std::vector<std::vector<std::tuple<uint64_t, std::string, std::string>>> result(kNumLevels);
    const auto version = vset_->current();
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto& file : version->files(level)) {
        result[static_cast<size_t>(level)].emplace_back(file->number,
                                                        std::string(file->smallest.Encode()),
                                                        std::string(file->largest.Encode()));
      }
    }
    return result;
  }

  // Writes a real table file with the given entries and returns its
  // metadata. Entries are (user key, sequence, type, value).
  FileMetaData BuildFile(
      const std::vector<std::tuple<std::string, SequenceNumber, ValueType, std::string>>& entries) {
    MemTable mem(icmp_);
    for (const auto& [key, sequence, type, value] : entries) {
      mem.Add(sequence, type, key, value);
    }
    FileMetaData meta;
    meta.number = vset_->NewFileNumber();
    auto iter = mem.NewIterator();
    EXPECT_OK(BuildTable(dbname_, env_, options_, &icmp_, table_cache_.get(), iter.get(), &meta));
    return meta;
  }

  std::string Get(std::string_view key) {
    std::string value;
    const Status s =
        vset_->current()->Get(ReadOptions(), LookupKey(key, kMaxSequenceNumber), &value);
    if (s.IsNotFound()) {
      return "NOT_FOUND";
    }
    return s.ok() ? value : s.ToString();
  }

  test::TempDir dir_;
  Env* const env_ = Env::Default();
  const std::string dbname_;
  Options options_;
  const InternalKeyComparator icmp_{BytewiseComparator()};
  std::mutex mu_;
  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<VersionSet> vset_;
};

TEST_F(VersionSetTest, NewDatabaseIsEmpty) {
  for (int level = 0; level < kNumLevels; ++level) {
    EXPECT_EQ(0, vset_->NumLevelFiles(level));
    EXPECT_EQ(0u, vset_->NumLevelBytes(level));
  }
  EXPECT_EQ(0u, vset_->LastSequence());
  EXPECT_EQ(0u, vset_->LogNumber());
  EXPECT_EQ(1u, vset_->ManifestFileNumber());
  EXPECT_EQ("files[ 0 0 0 0 0 0 0 ]", vset_->LevelSummary());
  EXPECT_EQ("NOT_FOUND", Get("anything"));
}

TEST_F(VersionSetTest, AppliedEditsSurviveRecovery) {
  vset_->MarkFileNumberUsed(20);
  vset_->SetLastSequence(500);
  VersionEdit add;
  add.SetLogNumber(15);
  add.AddFile(0, MakeMeta(10, "a", "m", 100));
  add.AddFile(0, MakeMeta(11, "c", "z", 200));
  add.AddFile(2, MakeMeta(12, "a", "f", 300));
  add.AddFile(2, MakeMeta(13, "g", "k", 400));
  ASSERT_OK(Apply(&add));
  EXPECT_EQ("files[ 2 0 2 0 0 0 0 ]", vset_->LevelSummary());
  EXPECT_EQ(700u, vset_->NumLevelBytes(2));
  const auto before = Files();

  ASSERT_OK(Reopen());
  EXPECT_EQ(before, Files());
  EXPECT_EQ(15u, vset_->LogNumber());
  EXPECT_EQ(500u, vset_->LastSequence());
  // Numbers handed out before the restart are never reused.
  EXPECT_GT(vset_->NewFileNumber(), 20u);

  VersionEdit remove;
  remove.RemoveFile(0, 10);
  remove.RemoveFile(2, 13);
  ASSERT_OK(Apply(&remove));
  ASSERT_OK(Reopen());
  EXPECT_EQ("files[ 1 0 1 0 0 0 0 ]", vset_->LevelSummary());
  EXPECT_EQ(11u, vset_->current()->files(0)[0]->number);
  EXPECT_EQ(12u, vset_->current()->files(2)[0]->number);
}

TEST_F(VersionSetTest, FirstUpdateStartsANewManifestAndLaterOnesAppend) {
  VersionEdit edit;
  edit.AddFile(0, MakeMeta(5, "a", "b"));
  ASSERT_OK(Apply(&edit));
  const uint64_t manifest = vset_->ManifestFileNumber();
  EXPECT_NE(1u, manifest);

  std::string current;
  ASSERT_OK(ReadFileToString(env_, CurrentFileName(dbname_), &current));
  EXPECT_EQ(DescriptorFileName(dbname_, manifest).substr(dbname_.size() + 1) + "\n", current);

  const uint64_t size_before = test::FileSize(DescriptorFileName(dbname_, manifest));
  VersionEdit more;
  more.AddFile(0, MakeMeta(6, "c", "d"));
  ASSERT_OK(Apply(&more));
  EXPECT_EQ(manifest, vset_->ManifestFileNumber());
  EXPECT_GT(test::FileSize(DescriptorFileName(dbname_, manifest)), size_before);
}

TEST_F(VersionSetTest, ManifestRollsOverPastTheSizeLimit) {
  options_.max_manifest_file_size = 1;  // Every update starts a new MANIFEST.
  ASSERT_OK(Reopen());
  std::set<uint64_t> manifests;
  for (uint64_t i = 0; i < 5; ++i) {
    VersionEdit edit;
    edit.AddFile(0, MakeMeta(100 + i, test::Key(i), test::Key(i)));
    ASSERT_OK(Apply(&edit));
    manifests.insert(vset_->ManifestFileNumber());
  }
  EXPECT_EQ(5u, manifests.size());
  const auto before = Files();
  ASSERT_OK(Reopen());
  EXPECT_EQ(before, Files());
  EXPECT_EQ(5, vset_->NumLevelFiles(0));
}

TEST_F(VersionSetTest, TornFinalManifestRecordIsIgnored) {
  VersionEdit first;
  first.AddFile(0, MakeMeta(10, "a", "b"));
  ASSERT_OK(Apply(&first));
  const std::string manifest = DescriptorFileName(dbname_, vset_->ManifestFileNumber());
  const uint64_t committed_size = test::FileSize(manifest);
  VersionEdit second;
  second.AddFile(0, MakeMeta(11, "c", "d"));
  ASSERT_OK(Apply(&second));
  const uint64_t full_size = test::FileSize(manifest);

  // A crash part way through appending the second edit: whatever part of
  // it made it to disk, recovery yields the state after the first edit.
  for (uint64_t cut = committed_size; cut < full_size; ++cut) {
    const std::string saved = test::ReadFileBytes(manifest);
    test::TruncateFile(manifest, cut);
    ASSERT_OK(Reopen()) << "cut " << cut;
    EXPECT_EQ(1, vset_->NumLevelFiles(0)) << "cut " << cut;
    EXPECT_EQ(10u, vset_->current()->files(0)[0]->number);
    test::WriteFileBytes(manifest, saved);
    // Reopen() above started no new MANIFEST: recovery itself never writes.
    ASSERT_EQ(manifest, DescriptorFileName(dbname_, vset_->ManifestFileNumber()));
  }
}

TEST_F(VersionSetTest, DamagedManifestRecordIsCorruption) {
  VersionEdit first;
  first.AddFile(0, MakeMeta(10, "a", "b"));
  ASSERT_OK(Apply(&first));
  VersionEdit second;
  second.AddFile(0, MakeMeta(11, "c", "d"));
  ASSERT_OK(Apply(&second));
  // The first record (the snapshot) has another record after it.
  test::CorruptByte(DescriptorFileName(dbname_, vset_->ManifestFileNumber()), 20);
  EXPECT_TRUE(Reopen().IsCorruption());
}

// MANIFEST edits are synced before anything acts on them (a flush deletes
// the log it replaced right after its edit commits), so a final record that
// is complete but damaged is not an edit that never committed. Dropping it
// would lose the tables it added; recovery must refuse instead.
TEST_F(VersionSetTest, DamagedFinalManifestRecordIsCorruption) {
  VersionEdit first;
  first.AddFile(0, MakeMeta(10, "a", "b"));
  ASSERT_OK(Apply(&first));
  VersionEdit second;
  second.AddFile(0, MakeMeta(11, "c", "d"));
  ASSERT_OK(Apply(&second));
  const std::string manifest = DescriptorFileName(dbname_, vset_->ManifestFileNumber());
  const uint64_t size = test::FileSize(manifest);
  for (uint64_t offset = size - 3; offset < size; ++offset) {
    const std::string saved = test::ReadFileBytes(manifest);
    test::CorruptByte(manifest, offset, 0x01);
    EXPECT_TRUE(Reopen().IsCorruption()) << "byte " << offset;
    test::WriteFileBytes(manifest, saved);
  }
  ASSERT_OK(Reopen());
  EXPECT_EQ(2, vset_->NumLevelFiles(0));
}

// The log reader accepts a checksum failure in a final record as zero fill
// from power loss when the record ends in zeros that start on a 512-byte
// boundary. Edits end in a non-zero marker, so wherever the final edit
// lands in the file, damage to it is reported.
TEST_F(VersionSetTest, DamagedFinalEditIsCorruptionAtEveryAlignment) {
  VersionEdit initial;
  initial.SetComparatorName(BytewiseComparator()->Name());
  initial.SetLogNumber(0);
  initial.SetNextFile(100);
  initial.SetLastSequence(1000);
  std::string initial_record;
  initial.EncodeTo(&initial_record);
  VersionEdit last;
  last.AddFile(0, MakeMeta(11, "c", "d"));
  std::string last_record;
  last.EncodeTo(&last_record);
  WriteManifest(9, {initial_record});  // Points CURRENT at it once.
  const std::string manifest = DescriptorFileName(dbname_, 9);
  for (size_t pad = 0; pad < 512; ++pad) {
    // A filler edit whose length shifts the final edit by one byte per step.
    VersionEdit filler;
    filler.AddFile(1, MakeMeta(10, "k" + std::string(pad, 'p'), "m"));
    std::string filler_record;
    filler.EncodeTo(&filler_record);
    std::string contents;
    {
      log::Writer writer(std::make_unique<test::StringSink>(&contents));
      ASSERT_OK(writer.AddRecord(initial_record));
      ASSERT_OK(writer.AddRecord(filler_record));
      ASSERT_OK(writer.AddRecord(last_record));
    }
    // The final edit's file number, ahead of the zeros in its keys.
    const size_t flip = contents.size() - last_record.size() + 2;
    test::WriteFileBytes(manifest, contents);
    ASSERT_OK(Reopen()) << "pad " << pad;
    contents[flip] = static_cast<char>(contents[flip] ^ 0x01);
    test::WriteFileBytes(manifest, contents);
    EXPECT_TRUE(Reopen().IsCorruption()) << "pad " << pad;
  }
}

// A MANIFEST rollover whose CURRENT rename succeeded but whose directory
// sync failed leaves CURRENT (possibly) naming the new MANIFEST. Later
// updates must not be appended to the old one, which recovery would then
// never read.
TEST_F(VersionSetTest, UpdatesAfterAFailedRolloverAreNotLostInAnUnreadManifest) {
  test::FaultInjectionEnv fault_env;
  options_.env = &fault_env;
  ASSERT_OK(Reopen());
  VersionEdit first;
  first.AddFile(0, MakeMeta(10, "a", "b"));
  ASSERT_OK(Apply(&first));

  options_.max_manifest_file_size = 1;  // The next update rolls over...
  fault_env.FailDirectorySyncsAfter(0);  // ...and fails after renaming CURRENT.
  VersionEdit second;
  second.AddFile(0, MakeMeta(11, "c", "d"));
  EXPECT_TRUE(Apply(&second).IsIOError());
  fault_env.ClearFailures();
  options_.max_manifest_file_size = 4 << 20;

  VersionEdit third;
  third.AddFile(0, MakeMeta(12, "e", "f"));
  ASSERT_OK(Apply(&third));
  const auto expected = Files();
  ASSERT_OK(Reopen());
  EXPECT_EQ(expected, Files());
  EXPECT_EQ(2, vset_->NumLevelFiles(0));
  options_.env = env_;
  ASSERT_OK(Reopen());
}

TEST_F(VersionSetTest, BadCurrentFileIsCorruption) {
  const std::string current = CurrentFileName(dbname_);
  test::WriteFileBytes(current, "MANIFEST-000001");  // No newline.
  EXPECT_TRUE(Reopen().IsCorruption());
  test::WriteFileBytes(current, "000001.log\n");
  EXPECT_TRUE(Reopen().IsCorruption());
  test::WriteFileBytes(current, "MANIFEST-000777\n");
  EXPECT_TRUE(Reopen().IsCorruption());
  test::WriteFileBytes(current, "MANIFEST-000001\n");
  EXPECT_OK(Reopen());
}

TEST_F(VersionSetTest, ManifestWithoutCountersIsCorruption) {
  VersionEdit edit;
  edit.SetComparatorName(BytewiseComparator()->Name());
  edit.SetLogNumber(0);  // No next-file number or last sequence.
  std::string record;
  edit.EncodeTo(&record);
  WriteManifest(9, {record});
  EXPECT_TRUE(Reopen().IsCorruption());
}

TEST_F(VersionSetTest, ComparatorMismatchIsRejected) {
  WriteInitialManifest("some.OtherComparator");
  const Status s = Reopen();
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(VersionSetTest, StructuralInvariantsAreEnforced) {
  VersionEdit overlapping;
  overlapping.AddFile(1, MakeMeta(10, "a", "m"));
  overlapping.AddFile(1, MakeMeta(11, "k", "z"));
  EXPECT_TRUE(Apply(&overlapping).IsCorruption());
  EXPECT_EQ(0, vset_->NumLevelFiles(1));

  VersionEdit two_levels;
  two_levels.AddFile(1, MakeMeta(12, "a", "b"));
  two_levels.AddFile(2, MakeMeta(12, "a", "b"));
  EXPECT_TRUE(Apply(&two_levels).IsCorruption());

  // Level 0 files may overlap; that is what flushes produce.
  VersionEdit level0;
  level0.AddFile(0, MakeMeta(13, "a", "m"));
  level0.AddFile(0, MakeMeta(14, "k", "z"));
  EXPECT_OK(Apply(&level0));

  // A failed update left nothing behind: the state recovers cleanly.
  ASSERT_OK(Reopen());
  EXPECT_EQ("files[ 2 0 0 0 0 0 0 ]", vset_->LevelSummary());
}

TEST_F(VersionSetTest, LiveFilesIncludeVersionsStillInUse) {
  VersionEdit add;
  add.AddFile(0, MakeMeta(10, "a", "b"));
  ASSERT_OK(Apply(&add));
  std::shared_ptr<Version> reader = vset_->current();  // An in-flight read.

  VersionEdit replace;
  replace.RemoveFile(0, 10);
  replace.AddFile(1, MakeMeta(11, "a", "b"));
  ASSERT_OK(Apply(&replace));

  std::set<uint64_t> live;
  vset_->AddLiveFiles(&live);
  EXPECT_EQ((std::set<uint64_t>{10, 11}), live);

  reader.reset();
  live.clear();
  vset_->AddLiveFiles(&live);
  EXPECT_EQ((std::set<uint64_t>{11}), live);
}

// Lookups go through real table files: level 0 newest first, then the
// sorted run of level 1, where at most one file can hold a key.
TEST_F(VersionSetTest, GetAndIteratorsSearchLevelsNewestFirst) {
  const FileMetaData l1_low = BuildFile({{"a", 1, kTypeValue, "a@1"},
                                         {"c", 2, kTypeValue, "c@2"},
                                         {"d", 3, kTypeValue, "d@3"},
                                         {"e", 4, kTypeValue, "e@4"}});
  const FileMetaData l1_high =
      BuildFile({{"m", 5, kTypeValue, "m@5"}, {"q", 6, kTypeValue, "q@6"}});
  const FileMetaData l0_old = BuildFile({{"c", 7, kTypeValue, "c@7"}, {"m", 8, kTypeDeletion, ""}});
  const FileMetaData l0_new =
      BuildFile({{"c", 9, kTypeValue, "c@9"}, {"z", 10, kTypeValue, "z@10"}});
  VersionEdit edit;
  edit.AddFile(1, l1_low);
  edit.AddFile(1, l1_high);
  edit.AddFile(0, l0_old);
  edit.AddFile(0, l0_new);
  vset_->SetLastSequence(10);
  ASSERT_OK(Apply(&edit));

  EXPECT_EQ("a@1", Get("a"));
  EXPECT_EQ("c@9", Get("c"));   // Newest level-0 file wins.
  EXPECT_EQ("d@3", Get("d"));
  EXPECT_EQ("NOT_FOUND", Get("m"));  // Tombstone in level 0 hides level 1.
  EXPECT_EQ("q@6", Get("q"));
  EXPECT_EQ("z@10", Get("z"));
  EXPECT_EQ("NOT_FOUND", Get("b"));  // In a file's range, but absent.
  EXPECT_EQ("NOT_FOUND", Get("h"));  // Between level-1 files.
  EXPECT_EQ("NOT_FOUND", Get(""));
  EXPECT_EQ("NOT_FOUND", Get("zz"));

  std::vector<std::unique_ptr<Iterator>> children;
  vset_->current()->AddIterators(ReadOptions(), &children);
  EXPECT_EQ(3u, children.size());  // Two level-0 files plus one for level 1.
  auto iter = NewDBIterator(NewMergingIterator(&icmp_, std::move(children)), BytewiseComparator(),
                            kMaxSequenceNumber);
  std::map<std::string, std::string> scanned;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    scanned.emplace(iter->key(), iter->value());
  }
  ASSERT_OK(iter->status());
  EXPECT_EQ((std::map<std::string, std::string>{
                {"a", "a@1"}, {"c", "c@9"}, {"d", "d@3"},
                {"e", "e@4"}, {"q", "q@6"}, {"z", "z@10"}}),
            scanned);

  // Survives recovery, reading the same files.
  ASSERT_OK(Reopen());
  EXPECT_EQ("c@9", Get("c"));
  EXPECT_EQ("NOT_FOUND", Get("m"));
}

TEST_F(VersionSetTest, MissingTableFileIsCorruptionNotNotFound) {
  const FileMetaData file = BuildFile({{"k", 1, kTypeValue, "v"}});
  VersionEdit edit;
  edit.AddFile(0, file);
  ASSERT_OK(Apply(&edit));
  ASSERT_OK(Reopen());  // Drops the cached open table.
  ASSERT_OK(env_->RemoveFile(TableFileName(dbname_, file.number)));
  std::string value;
  const Status s = vset_->current()->Get(ReadOptions(), LookupKey("k", 1), &value);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// ---------------------------------------------------------------------------
// Compaction picking. These need only metadata, so the files do not exist.

std::vector<uint64_t> Numbers(const std::vector<std::shared_ptr<FileMetaData>>& files) {
  std::vector<uint64_t> numbers;
  for (const auto& file : files) {
    numbers.push_back(file->number);
  }
  return numbers;
}

TEST_F(VersionSetTest, MaxBytesGrowByTheMultiplierPerLevel) {
  options_.max_bytes_for_level_base = 1000;
  options_.max_bytes_for_level_multiplier = 10;
  EXPECT_EQ(1000u, vset_->MaxBytesForLevel(1));
  EXPECT_EQ(10000u, vset_->MaxBytesForLevel(2));
  EXPECT_EQ(1000000000u, vset_->MaxBytesForLevel(7));
  options_.max_bytes_for_level_base = uint64_t{1} << 40;
  options_.max_bytes_for_level_multiplier = 100;
  // Saturates instead of overflowing.
  EXPECT_GT(vset_->MaxBytesForLevel(6), uint64_t{1} << 62);
}

TEST_F(VersionSetTest, CompactionIsNeededAtTheLevel0TriggerOrOverALevelLimit) {
  options_.level0_compaction_trigger = 4;
  options_.max_bytes_for_level_base = 10000;
  VersionEdit edit;
  for (uint64_t n = 10; n < 13; ++n) {
    edit.AddFile(0, MakeMeta(n, "a", "z"));
  }
  ASSERT_OK(Apply(&edit));
  EXPECT_FALSE(vset_->NeedsCompaction());
  EXPECT_EQ(nullptr, vset_->PickCompaction());

  edit.Clear();
  edit.AddFile(0, MakeMeta(13, "a", "z"));
  ASSERT_OK(Apply(&edit));
  EXPECT_TRUE(vset_->NeedsCompaction());
  EXPECT_EQ(0, vset_->current()->compaction_level());

  // Level 2 holds 3x its limit, level 0 is at 1x: level 2 wins.
  edit.Clear();
  edit.AddFile(2, MakeMeta(20, "a", "m", 150000));
  edit.AddFile(2, MakeMeta(21, "n", "z", 150000));
  ASSERT_OK(Apply(&edit));
  EXPECT_EQ(2, vset_->current()->compaction_level());
  EXPECT_DOUBLE_EQ(3.0, vset_->current()->compaction_score());

  // The last level is never compacted, however big.
  edit.Clear();
  for (uint64_t n = 10; n < 14; ++n) edit.RemoveFile(0, n);
  edit.RemoveFile(2, 20);
  edit.RemoveFile(2, 21);
  edit.AddFile(kNumLevels - 1, MakeMeta(30, "a", "z", uint64_t{1} << 40));
  ASSERT_OK(Apply(&edit));
  EXPECT_FALSE(vset_->NeedsCompaction());
}

TEST_F(VersionSetTest, Level0CompactionPullsInEveryOverlappingFile) {
  options_.level0_compaction_trigger = 4;
  VersionEdit edit;
  edit.AddFile(0, MakeMeta(10, "a", "c"));  // Oldest: picked first.
  edit.AddFile(0, MakeMeta(11, "b", "e"));  // Overlaps 10.
  edit.AddFile(0, MakeMeta(12, "x", "z"));  // Overlaps nothing picked.
  edit.AddFile(0, MakeMeta(13, "d", "f"));  // Overlaps 11 only.
  edit.AddFile(1, MakeMeta(20, "a", "b"));
  edit.AddFile(1, MakeMeta(21, "e", "g"));
  edit.AddFile(1, MakeMeta(22, "h", "k"));
  edit.AddFile(2, MakeMeta(30, "f", "j"));
  edit.AddFile(2, MakeMeta(31, "l", "m"));
  ASSERT_OK(Apply(&edit));

  const std::unique_ptr<Compaction> c = vset_->PickCompaction();
  ASSERT_NE(nullptr, c);
  EXPECT_EQ(0, c->level());
  EXPECT_EQ(1, c->output_level());
  EXPECT_EQ((std::vector<uint64_t>{10, 11, 13}), Numbers(c->inputs(0)));
  EXPECT_EQ((std::vector<uint64_t>{20, 21}), Numbers(c->inputs(1)));
  EXPECT_FALSE(c->IsTrivialMove());
  EXPECT_EQ(5000u, c->TotalInputBytes());

  VersionEdit deletions;
  c->AddInputDeletions(&deletions);
  EXPECT_NE(std::string::npos, deletions.DebugString().find("13"));
}

TEST_F(VersionSetTest, DeeperLevelsArePickedRoundRobin) {
  options_.max_bytes_for_level_base = 2500;
  VersionEdit edit;
  edit.AddFile(1, MakeMeta(10, "a", "b"));
  edit.AddFile(1, MakeMeta(11, "c", "d"));
  edit.AddFile(1, MakeMeta(12, "e", "f"));
  edit.AddFile(2, MakeMeta(20, "c", "c"));
  ASSERT_OK(Apply(&edit));
  ASSERT_EQ(1, vset_->current()->compaction_level());

  std::vector<uint64_t> picked;
  std::vector<bool> trivial;
  for (int i = 0; i < 4; ++i) {
    const std::unique_ptr<Compaction> c = vset_->PickCompaction();
    ASSERT_NE(nullptr, c);
    ASSERT_EQ(1, c->num_input_files(0));
    picked.push_back(c->inputs(0)[0]->number);
    trivial.push_back(c->IsTrivialMove());
  }
  // Each pick starts after the previous one, wrapping around.
  EXPECT_EQ((std::vector<uint64_t>{10, 11, 12, 10}), picked);
  // Only file 11 overlaps the next level.
  EXPECT_EQ((std::vector<bool>{true, false, true, true}), trivial);
}

TEST_F(VersionSetTest, TombstonesAreDroppedOnlyBelowEveryFileHoldingTheKey) {
  VersionEdit edit;
  edit.AddFile(1, MakeMeta(10, "a", "z"));
  edit.AddFile(3, MakeMeta(30, "d", "f"));
  edit.AddFile(4, MakeMeta(40, "m", "p"));
  edit.AddFile(4, MakeMeta(41, "r", "s"));
  ASSERT_OK(Apply(&edit));

  // Level 1 into level 2: levels 3 and up are below the output.
  std::unique_ptr<Compaction> c = vset_->CompactRange(1, nullptr, nullptr);
  ASSERT_NE(nullptr, c);
  std::vector<bool> base;
  for (const char* key : {"a", "d", "e", "g", "m", "o", "q", "r", "t"}) {
    base.push_back(c->IsBaseLevelForKey(key));
  }
  EXPECT_EQ((std::vector<bool>{true, false, false, true, false, false, true, false, true}), base);

  // Level 3 into level 4: only file 30 itself is an input; nothing lies
  // below level 5 for these keys.
  c = vset_->CompactRange(3, nullptr, nullptr);
  ASSERT_NE(nullptr, c);
  EXPECT_EQ((std::vector<uint64_t>{30}), Numbers(c->inputs(0)));
  EXPECT_TRUE(c->inputs(1).empty());
  EXPECT_TRUE(c->IsBaseLevelForKey("e"));
}

TEST_F(VersionSetTest, OverlappingInputsHonourTheRangeBounds) {
  VersionEdit edit;
  edit.AddFile(1, MakeMeta(10, "a", "c"));
  edit.AddFile(1, MakeMeta(11, "e", "g"));
  edit.AddFile(1, MakeMeta(12, "i", "k"));
  ASSERT_OK(Apply(&edit));
  const auto version = vset_->current();
  std::vector<std::shared_ptr<FileMetaData>> inputs;
  const InternalKey d("d", kMaxSequenceNumber, kValueTypeForSeek);
  const InternalKey f("f", 0, kTypeDeletion);
  const InternalKey i_key("i", 0, kTypeDeletion);
  version->GetOverlappingInputs(1, &d, &f, &inputs);
  EXPECT_EQ((std::vector<uint64_t>{11}), Numbers(inputs));
  version->GetOverlappingInputs(1, nullptr, &f, &inputs);
  EXPECT_EQ((std::vector<uint64_t>{10, 11}), Numbers(inputs));
  version->GetOverlappingInputs(1, &d, nullptr, &inputs);
  EXPECT_EQ((std::vector<uint64_t>{11, 12}), Numbers(inputs));
  version->GetOverlappingInputs(1, &i_key, &i_key, &inputs);
  EXPECT_EQ((std::vector<uint64_t>{12}), Numbers(inputs));
  EXPECT_EQ(nullptr, vset_->CompactRange(2, nullptr, nullptr));

  const std::string_view h = "h";
  const std::string_view z = "z";
  EXPECT_FALSE(version->OverlapInLevel(1, &h, &h));
  EXPECT_TRUE(version->OverlapInLevel(1, &h, &z));
  EXPECT_TRUE(version->OverlapInLevel(1, nullptr, nullptr));
  EXPECT_FALSE(version->OverlapInLevel(2, nullptr, nullptr));
}

}  // namespace
}  // namespace kvlog
