add_executable(xrdoucutils-unit-tests XrdOucUtilsTests.cc)

target_link_libraries(xrdoucutils-unit-tests XrdUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdoucutils-unit-tests
  PROPERTIES DISCOVERY_TIMEOUT 10)

add_executable(xrdoucstring-memory-tests XrdOucStringMemoryTests.cc)

target_link_libraries(xrdoucstring-memory-tests XrdUtils)

add_test(NAME xrdoucstring-memory-tests COMMAND xrdoucstring-memory-tests)

add_executable(xrdouc-gmap-parse-tests XrdOucGMapParseTests.cc)

target_link_libraries(xrdouc-gmap-parse-tests XrdUtils)

add_test(NAME xrdouc-gmap-parse-tests COMMAND xrdouc-gmap-parse-tests)
