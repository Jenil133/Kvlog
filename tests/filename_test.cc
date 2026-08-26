#include "db/filename.h"

#include <string>

#include <gtest/gtest.h>

#include "test_util.h"
#include "util/env_util.h"

namespace kvlog {
namespace {

TEST(FileNameTest, ParsesNamesTheDatabaseCreates) {
  struct Case {
    const char* name;
    uint64_t number;
    FileType type;
  };
  const Case cases[] = {
      {"100.log", 100, FileType::kLogFile},
      {"0.log", 0, FileType::kLogFile},
      {"000123.sst", 123, FileType::kTableFile},
      {"000077.tmp", 77, FileType::kTempFile},
      {"CURRENT", 0, FileType::kCurrentFile},
      {"LOCK", 0, FileType::kLockFile},
      {"MANIFEST-2", 2, FileType::kDescriptorFile},
      {"MANIFEST-7", 7, FileType::kDescriptorFile},
      {"18446744073709551615.log", 18446744073709551615ull, FileType::kLogFile},
  };
  for (const Case& c : cases) {
    uint64_t number = 999;
    FileType type = FileType::kTempFile;
    ASSERT_TRUE(ParseFileName(c.name, &number, &type)) << c.name;
    EXPECT_EQ(c.number, number) << c.name;
    EXPECT_EQ(c.type, type) << c.name;
  }
}

TEST(FileNameTest, RejectsForeignNames) {
  const char* const bad[] = {
      "",
      "foo",
      "foo-dx-100.log",
      ".log",
      "manifest",
      "CURREN",
      "CURRENTX",
      "MANIFES",
      "MANIFEST",
      "MANIFEST-",
      "XMANIFEST-3",
      "MANIFEST-3x",
      "LOC",
      "LOCKx",
      "LO",
      "LOGx",
      "18446744073709551616.log",  // Overflows 64 bits.
      "184467440737095516150.log",
      "100",
      "100.",
      "100.lop",
      "100.sst.bak",
      "-1.log",
  };
  for (const char* name : bad) {
    uint64_t number = 0;
    FileType type;
    EXPECT_FALSE(ParseFileName(name, &number, &type)) << name;
  }
}

TEST(FileNameTest, ConstructedNamesParseBack) {
  struct Case {
    std::string path;
    uint64_t number;
    FileType type;
  };
  const Case cases[] = {
      {LogFileName("db", 192), 192, FileType::kLogFile},
      {TableFileName("db", 200), 200, FileType::kTableFile},
      {TempFileName("db", 999), 999, FileType::kTempFile},
      {DescriptorFileName("db", 100), 100, FileType::kDescriptorFile},
      {CurrentFileName("db"), 0, FileType::kCurrentFile},
      {LockFileName("db"), 0, FileType::kLockFile},
  };
  for (const Case& c : cases) {
    ASSERT_TRUE(c.path.starts_with("db/")) << c.path;
    uint64_t number = 0;
    FileType type;
    ASSERT_TRUE(ParseFileName(c.path.substr(3), &number, &type)) << c.path;
    EXPECT_EQ(c.number, number);
    EXPECT_EQ(c.type, type);
  }
  EXPECT_EQ("db/000192.log", LogFileName("db", 192));
  EXPECT_EQ("db/MANIFEST-000100", DescriptorFileName("db", 100));
}

TEST(FileNameTest, SetCurrentFileReplacesCurrentAtomically) {
  test::TempDir dir;
  Env* env = Env::Default();
  ASSERT_OK(SetCurrentFile(env, dir.path(), 5));
  std::string contents;
  ASSERT_OK(ReadFileToString(env, CurrentFileName(dir.path()), &contents));
  EXPECT_EQ("MANIFEST-000005\n", contents);

  ASSERT_OK(SetCurrentFile(env, dir.path(), 12));
  ASSERT_OK(ReadFileToString(env, CurrentFileName(dir.path()), &contents));
  EXPECT_EQ("MANIFEST-000012\n", contents);
  // The temporary file was renamed away, not left behind.
  EXPECT_EQ(std::vector<std::string>{"CURRENT"}, test::ListDir(dir.path()));
}

}  // namespace
}  // namespace kvlog
