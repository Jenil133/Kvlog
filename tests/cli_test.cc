#include "cli.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kvlog/db.h"
#include "test_util.h"

namespace kvlog::cli {
namespace {

class CliTest : public testing::Test {
 protected:
  struct Result {
    int code;
    std::string out;
    std::string err;
  };

  Result Run(std::vector<std::string> args) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::Run(args, out, err);
    return Result{code, out.str(), err.str()};
  }

  // Runs a command against the test database.
  Result Db(std::vector<std::string> args) {
    args.insert(args.begin(), dbname_);
    return Run(std::move(args));
  }

  test::TempDir dir_;
  const std::string dbname_ = dir_.Sub("db");
};

TEST_F(CliTest, PutGetDeleteRoundTrip) {
  EXPECT_EQ(kExitOk, Db({"put", "fruit", "apple"}).code);
  Result r = Db({"get", "fruit"});
  EXPECT_EQ(kExitOk, r.code);
  EXPECT_EQ("apple\n", r.out);

  EXPECT_EQ(kExitOk, Db({"put", "fruit", "pear"}).code);
  EXPECT_EQ("pear\n", Db({"get", "fruit"}).out);

  EXPECT_EQ(kExitOk, Db({"delete", "fruit"}).code);
  r = Db({"get", "fruit"});
  EXPECT_EQ(kExitNotFound, r.code);
  EXPECT_EQ("", r.out);
  EXPECT_NE(std::string::npos, r.err.find("not found"));
}

TEST_F(CliTest, ScanPrintsSortedRangesWithLimits) {
  for (const char* key : {"c", "a", "e", "b", "d"}) {
    ASSERT_EQ(kExitOk, Db({"put", key, std::string("v") + key}).code);
  }
  ASSERT_EQ(kExitOk, Db({"flush"}).code);
  ASSERT_EQ(kExitOk, Db({"put", "f", "vf"}).code);  // One key only in the log.

  EXPECT_EQ("a\tva\nb\tvb\nc\tvc\nd\tvd\ne\tve\nf\tvf\n", Db({"scan"}).out);
  EXPECT_EQ("c\tvc\nd\tvd\ne\tve\nf\tvf\n", Db({"scan", "c"}).out);
  EXPECT_EQ("b\tvb\nc\tvc\n", Db({"scan", "b", "d"}).out);  // End is exclusive.
  EXPECT_EQ("a\tva\nb\tvb\n", Run({"--limit=2", dbname_, "scan"}).out);
  EXPECT_EQ("", Db({"scan", "x"}).out);
}

TEST_F(CliTest, BinaryAndBackslashBytesAreEscaped) {
  EXPECT_EQ("plain", Escape("plain"));
  EXPECT_EQ("a\\\\b", Escape("a\\b"));
  EXPECT_EQ("\\x00\\xff\\x0a", Escape(std::string("\0\xff\n", 3)));
  ASSERT_EQ(kExitOk, Db({"put", "k", std::string("tab\there", 8)}).code);
  EXPECT_EQ("tab\\x09here\n", Db({"get", "k"}).out);
}

TEST_F(CliTest, SyncFlushAndStats) {
  EXPECT_EQ(kExitOk, Run({"--sync", dbname_, "put", "k", "v"}).code);
  EXPECT_EQ(kExitOk, Db({"flush"}).code);
  const Result stats = Db({"stats"});
  EXPECT_EQ(kExitOk, stats.code);
  EXPECT_NE(std::string::npos, stats.out.find("Level  Files  Bytes")) << stats.out;
  EXPECT_EQ("v\n", Db({"get", "k"}).out);
}

TEST_F(CliTest, ReadCommandsDoNotCreateADatabase) {
  const Result r = Db({"get", "k"});
  EXPECT_EQ(kExitError, r.code);
  EXPECT_NE(std::string::npos, r.err.find("does not exist")) << r.err;
  EXPECT_TRUE(test::ListDir(dir_.path()).empty());
  EXPECT_EQ(kExitError, Db({"scan"}).code);
  EXPECT_TRUE(test::ListDir(dir_.path()).empty());
}

TEST_F(CliTest, UsageErrors) {
  EXPECT_EQ(kExitUsage, Run({}).code);
  EXPECT_EQ(kExitUsage, Run({dbname_}).code);
  EXPECT_EQ(kExitUsage, Db({"frobnicate"}).code);
  EXPECT_EQ(kExitUsage, Db({"put", "only-a-key"}).code);
  EXPECT_EQ(kExitUsage, Db({"get"}).code);
  EXPECT_EQ(kExitUsage, Db({"scan", "a", "b", "c"}).code);
  EXPECT_EQ(kExitUsage, Run({"--bogus", dbname_, "scan"}).code);
  EXPECT_EQ(kExitUsage, Run({"--limit=", dbname_, "scan"}).code);
  EXPECT_EQ(kExitUsage, Run({"--limit=12x", dbname_, "scan"}).code);
  const Result usage = Run({"put", "only-a-key"});
  EXPECT_NE(std::string::npos, usage.err.find("usage:")) << usage.err;
  EXPECT_TRUE(test::ListDir(dir_.path()).empty());  // No usage error touched the disk.

  const Result help = Run({"--help"});
  EXPECT_EQ(kExitOk, help.code);
  EXPECT_NE(std::string::npos, help.out.find("usage:"));
}

TEST_F(CliTest, LockedDatabaseIsReportedAsBusy) {
  ASSERT_EQ(kExitOk, Db({"put", "k", "v"}).code);
  Options options;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, dbname_, &db));
  const Result r = Db({"get", "k"});
  EXPECT_EQ(kExitError, r.code);
  EXPECT_NE(std::string::npos, r.err.find("Busy")) << r.err;
}

}  // namespace
}  // namespace kvlog::cli
