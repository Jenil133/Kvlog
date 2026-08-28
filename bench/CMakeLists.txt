add_executable(kvlog_bench kvlog_bench.cc)
target_link_libraries(kvlog_bench PRIVATE kvlog kvlog_warnings)

if(KVLOG_BUILD_TESTS)
  # Unit tests for the key generators and percentile code.
  add_executable(kvlog_bench_tests workload_test.cc)
  target_link_libraries(kvlog_bench_tests PRIVATE GTest::gtest_main kvlog_warnings)
  target_include_directories(kvlog_bench_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  gtest_discover_tests(kvlog_bench_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

  # Tiny end-to-end runs (a few thousand keys): the benchmark works and its
  # read results are correct. Not a measurement.
  set(KVLOG_BENCH_SMOKE_DIR ${CMAKE_CURRENT_BINARY_DIR}/smoke)
  add_test(NAME kvlog_bench_smoke_benchmarks
    COMMAND kvlog_bench --db=${KVLOG_BENCH_SMOKE_DIR}/list --num=3000 --ops=3000
            --write_buffer_size=65536 --verify=1 --stats=1
            --benchmarks=fillrandom,readrandom,readmissing,mixed90,mixed50,fillseq,readrandom)
  add_test(NAME kvlog_bench_smoke_zipfian
    COMMAND kvlog_bench --db=${KVLOG_BENCH_SMOKE_DIR}/zipf --num=3000 --ops=3000
            --write_buffer_size=65536 --key_dist=zipfian --verify=1
            --benchmarks=fillseq,readrandom,readmissing,mixed50)
  add_test(NAME kvlog_bench_smoke_growth
    COMMAND kvlog_bench --db=${KVLOG_BENCH_SMOKE_DIR}/growth --growth=1 --ops=2000
            --growth_step_bytes=131072 --growth_max_bytes=524288 --write_buffer_size=65536
            --block_cache_bytes=65536 --key_dist=zipfian
            --csv=${KVLOG_BENCH_SMOKE_DIR}/growth.csv)
  set_tests_properties(kvlog_bench_smoke_benchmarks kvlog_bench_smoke_zipfian
                       kvlog_bench_smoke_growth PROPERTIES TIMEOUT 300)
  file(MAKE_DIRECTORY ${KVLOG_BENCH_SMOKE_DIR})
endif()
