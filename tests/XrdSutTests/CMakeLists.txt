add_executable(xrdsut-cacheentry-tests XrdSutCacheEntryTests.cc)

target_link_libraries(xrdsut-cacheentry-tests XrdUtils)

add_test(NAME xrdsut-cacheentry-tests COMMAND xrdsut-cacheentry-tests)
