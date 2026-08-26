// Space amplification: an overwrite- and delete-heavy workload writes about
// 20 times the size of the data that stays live. With compaction, the bytes
// on disk (tables plus write-ahead logs) stay within 3x the live data; with
// automatic compaction disabled they grow with every write.

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "db_test_util.h"
#include "test_util.h"

namespace kvlog {
namespace {

using test::Key;

constexpr uint64_t kKeySpace = 3000;
constexpr size_t kValueSize = 100;
constexpr int kOps = 60000;  // 3/4 puts: about 20x the live set in bytes.
constexpr int kCheckpoints = 4;

struct Measurement {
  uint64_t live_bytes = 0;  // Keys plus values of the live entries.
  uint64_t disk_bytes[kCheckpoints] = {};  // Tables plus logs, at each quarter.
  uint64_t written_bytes = 0;  // Keys plus values of every put.
};

class SpaceAmpTest : public test::DBTestBase {
 protected:
  void SetUp() override {
    UseNoSyncEnv();
    options_.write_buffer_size = 32 << 10;
    options_.target_file_size = 32 << 10;
    options_.max_bytes_for_level_base = 64 << 10;
    options_.max_bytes_for_level_multiplier = 10;
    options_.level0_compaction_trigger = 4;
    ASSERT_OK(Reopen());
  }

  uint64_t DiskBytes() const {
    uint64_t total = 0;
    for (const char* suffix : {".sst", ".log"}) {
      for (const std::string& file : DbFiles(suffix)) {
        total += test::FileSize(file);
      }
    }
    return total;
  }

  void RunWorkload(Measurement* m) {
    std::mt19937_64 rng(42);
    std::map<std::string, std::string> model;
    for (int i = 0; i < kOps; ++i) {
      const std::string key = Key(rng() % kKeySpace);
      if (rng() % 4 == 0) {
        ASSERT_OK(Delete(key));
        model.erase(key);
      } else {
        std::string value = test::RandomBytes(&rng, kValueSize);
        m->written_bytes += key.size() + value.size();
        ASSERT_OK(Put(key, value));
        model[key] = std::move(value);
      }
      if ((i + 1) % (kOps / kCheckpoints) == 0) {
        // Measured once background work has caught up, so the numbers do
        // not depend on how far the background thread happened to get.
        db_->WaitForBackgroundWork();
        m->disk_bytes[(i + 1) / (kOps / kCheckpoints) - 1] = DiskBytes();
      }
    }
    for (const auto& [key, value] : model) {
      m->live_bytes += key.size() + value.size();
    }
    ASSERT_EQ(model, Scan());
  }

  static void Print(const char* name, const Measurement& m) {
    std::printf("%s: live %llu B, written %llu B (%.1fx live), on disk at each quarter:", name,
                static_cast<unsigned long long>(m.live_bytes),
                static_cast<unsigned long long>(m.written_bytes),
                static_cast<double>(m.written_bytes) / static_cast<double>(m.live_bytes));
    for (const uint64_t bytes : m.disk_bytes) {
      std::printf(" %llu B (%.2fx)", static_cast<unsigned long long>(bytes),
                  static_cast<double>(bytes) / static_cast<double>(m.live_bytes));
    }
    std::printf("\n");
  }
};

TEST_F(SpaceAmpTest, CompactionKeepsDiskUseNearTheLiveDataSize) {
  Measurement m;
  RunWorkload(&m);
  Print("with compaction", m);
  std::string stats;
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  std::printf("%s", stats.c_str());

  ASSERT_GT(m.written_bytes, 18 * m.live_bytes);  // The workload is what it claims.
  for (const uint64_t bytes : m.disk_bytes) {
    EXPECT_LE(bytes, 3 * m.live_bytes);
  }
}

TEST_F(SpaceAmpTest, WithoutCompactionDiskUseKeepsGrowing) {
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  Measurement m;
  RunWorkload(&m);
  Print("without compaction", m);

  ASSERT_GT(m.written_bytes, 18 * m.live_bytes);
  for (int i = 1; i < kCheckpoints; ++i) {
    EXPECT_GT(m.disk_bytes[i], m.disk_bytes[i - 1]);
  }
  EXPECT_GT(m.disk_bytes[kCheckpoints - 1], 10 * m.live_bytes);
  EXPECT_GT(m.disk_bytes[kCheckpoints - 1], 3 * m.disk_bytes[0]);

  // A manual compaction brings it back down to the same bound.
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  const uint64_t compacted = DiskBytes();
  std::printf("after CompactRange: %llu B (%.2fx live)\n",
              static_cast<unsigned long long>(compacted),
              static_cast<double>(compacted) / static_cast<double>(m.live_bytes));
  EXPECT_LE(compacted, 3 * m.live_bytes);
}

}  // namespace
}  // namespace kvlog
