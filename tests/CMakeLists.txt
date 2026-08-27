include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Recent Clang flags a char8_t conversion inside GoogleTest's own sources.
# That is fetched code, so the diagnostic is silenced on its targets only.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wcharacter-conversion KVLOG_CXX_HAS_WCHARACTER_CONVERSION)
if(KVLOG_CXX_HAS_WCHARACTER_CONVERSION)
  target_compile_options(gtest PRIVATE -Wno-character-conversion)
  target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
endif()

include(GoogleTest)

# Helpers shared by every test binary. Tests also reach into src/ to unit
# test internal components.
add_library(kvlog_test_util STATIC test_util.cc subprocess.cc)
target_include_directories(kvlog_test_util PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(kvlog_test_util PUBLIC kvlog GTest::gtest PRIVATE kvlog_warnings)

# Writes to a database in a process of its own, so tests can kill it at
# arbitrary points and check what recovery restores.
add_executable(kvlog_crash_helper crash_helper.cc)
target_link_libraries(kvlog_crash_helper PRIVATE kvlog_test_util kvlog_warnings)

set(KVLOG_TEST_SOURCES
  arena_test.cc
  bloom_test.cc
  cache_test.cc
  coding_test.cc
  compaction_test.cc
  concurrency_test.cc
  crash_test.cc
  crc32c_test.cc
  db_test.cc
  dbformat_test.cc
  fault_injection_test.cc
  filename_test.cc
  filter_block_test.cc
  hash_test.cc
  iterator_test.cc
  log_test.cc
  memtable_test.cc
  power_loss_test.cc
  recovery_test.cc
  skiplist_test.cc
  space_amp_test.cc
  table_test.cc
  version_set_test.cc
  write_batch_test.cc
)

if(TARGET kvlog_cli_lib)
  list(APPEND KVLOG_TEST_SOURCES cli_test.cc)
endif()

add_executable(kvlog_tests ${KVLOG_TEST_SOURCES})
target_link_libraries(kvlog_tests PRIVATE kvlog_test_util GTest::gtest_main kvlog_warnings)
target_compile_definitions(kvlog_tests PRIVATE
  KVLOG_CRASH_HELPER_PATH="$<TARGET_FILE:kvlog_crash_helper>")
add_dependencies(kvlog_tests kvlog_crash_helper)
if(TARGET kvlog_cli_lib)
  target_link_libraries(kvlog_tests PRIVATE kvlog_cli_lib)
endif()
gtest_discover_tests(kvlog_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 300)

# Long randomized and crash-loop tests, run separately (ctest label "stress").
add_executable(kvlog_stress_tests stress_test.cc)
target_link_libraries(kvlog_stress_tests PRIVATE kvlog_test_util GTest::gtest_main kvlog_warnings)
target_compile_definitions(kvlog_stress_tests PRIVATE
  KVLOG_CRASH_HELPER_PATH="$<TARGET_FILE:kvlog_crash_helper>")
add_dependencies(kvlog_stress_tests kvlog_crash_helper)
gtest_discover_tests(kvlog_stress_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS stress TIMEOUT 1800)
