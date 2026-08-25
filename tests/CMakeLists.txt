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
add_library(kvlog_test_util STATIC test_util.cc)
target_include_directories(kvlog_test_util PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(kvlog_test_util PUBLIC kvlog GTest::gtest PRIVATE kvlog_warnings)

set(KVLOG_TEST_SOURCES
  arena_test.cc
  coding_test.cc
  crc32c_test.cc
  dbformat_test.cc
  hash_test.cc
  memtable_test.cc
  skiplist_test.cc
  write_batch_test.cc
)

add_executable(kvlog_tests ${KVLOG_TEST_SOURCES})
target_link_libraries(kvlog_tests PRIVATE kvlog_test_util GTest::gtest_main kvlog_warnings)
gtest_discover_tests(kvlog_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 300)
