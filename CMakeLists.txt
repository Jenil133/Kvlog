cmake_minimum_required(VERSION 3.20)

project(kvlog VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KVLOG_BUILD_TESTS "Build the GoogleTest suites" ON)
set(KVLOG_SANITIZER "" CACHE STRING "Sanitizer build: empty, 'address' (ASan + UBSan) or 'thread' (TSan)")
set_property(CACHE KVLOG_SANITIZER PROPERTY STRINGS "" address thread)

find_package(Threads REQUIRED)

# Sanitizers must instrument every object in the process (TSan reports false
# races otherwise), so they are applied globally, including to GoogleTest.
if(KVLOG_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(KVLOG_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -O1)
  add_link_options(-fsanitize=thread)
elseif(NOT KVLOG_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown KVLOG_SANITIZER '${KVLOG_SANITIZER}'")
endif()

# Warning flags apply to kvlog's own targets only, never to fetched code.
add_library(kvlog_warnings INTERFACE)
target_compile_options(kvlog_warnings INTERFACE -Wall -Wextra -Wpedantic)

add_library(kvlog STATIC
  src/db/builder.cc
  src/db/compaction.cc
  src/db/db_iter.cc
  src/db/dbformat.cc
  src/db/filename.cc
  src/db/log_reader.cc
  src/db/log_writer.cc
  src/db/memtable.cc
  src/db/table_cache.cc
  src/db/version_edit.cc
  src/db/version_set.cc
  src/db/write_batch.cc
  src/table/block.cc
  src/table/block_builder.cc
  src/table/filter_block.cc
  src/table/format.cc
  src/table/iterator.cc
  src/table/merging_iterator.cc
  src/table/table.cc
  src/table/table_builder.cc
  src/table/two_level_iterator.cc
  src/util/arena.cc
  src/util/bloom.cc
  src/util/cache.cc
  src/util/coding.cc
  src/util/comparator.cc
  src/util/crc32c.cc
  src/util/env.cc
  src/util/env_posix.cc
  src/util/hash.cc
  src/util/status.cc
)
target_include_directories(kvlog
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kvlog PUBLIC Threads::Threads PRIVATE kvlog_warnings)

if(KVLOG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
