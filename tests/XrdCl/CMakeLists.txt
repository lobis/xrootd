add_executable(xrdcl-unit-tests
  XrdClEnv.cc
  XrdClURL.cc
  XrdClPoller.cc
  XrdClSocket.cc
  XrdClUtilsTest.cc
  XrdClCopyBatchTest.cc
  XrdClCopyChecksumTest.cc
  XrdClProtocolRespTest.cc
  )

target_link_libraries(xrdcl-unit-tests
  XrdTestUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdcl-unit-tests TEST_PREFIX XrdCl:: 
  PROPERTIES DISCOVERY_TIMEOUT 10)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(copy_batch_http_plugin)
  if(TARGET XrdClHttp-${PLUGIN_VERSION})
    set(copy_batch_http_plugin $<TARGET_FILE:XrdClHttp-${PLUGIN_VERSION}>)
  endif()
  add_test(NAME XrdCl::CopyBatchCLI
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/copy_batch.py
      $<TARGET_FILE:xrdcp> ${copy_batch_http_plugin})
  set_tests_properties(XrdCl::CopyBatchCLI PROPERTIES TIMEOUT 120)
endif()

if(NOT ENABLE_SERVER_TESTS)
  return()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")

add_executable(xrdcl-cluster-tests
  IdentityPlugIn.cc
  # XrdClFileTest.cc
  # XrdClFileCopyTest.cc
  # XrdClFileSystemTest.cc
  # XrdClOperationsWorkflowTest.cc
  XrdClLocalFileHandlerTest.cc
  XrdClPostMasterTest.cc
  XrdClThreadingTest.cc
  XrdClZip.cc
  )

target_link_libraries(xrdcl-cluster-tests
  XrdTestUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdcl-cluster-tests TEST_PREFIX XrdCl::
  PROPERTIES DEPENDS XRootD_Cluster FIXTURES_REQUIRED XRootD_Cluster DISCOVERY_TIMEOUT 10)

# tests to be run in serial, otherwise they are problematic
set(SERIAL_TESTS_FILES
  XrdClFileTest.cc
  XrdClFileClone.cc
  XrdClFileCopyTest.cc
  XrdClFileSystemTest.cc
  XrdClOperationsWorkflowTest.cc
  XrdClTTLManyStreams.cc
)

# create a separate executable target for each "problematic" test suite
foreach(i ${SERIAL_TESTS_FILES})
  add_executable(${i}-tests ${i} IdentityPlugIn.cc)

  target_link_libraries(${i}-tests
    XrdTestUtils GTest::gtest GTest::gtest_main)

  gtest_discover_tests(${i}-tests TEST_PREFIX XrdCl::
    PROPERTIES DEPENDS XRootD_Cluster FIXTURES_REQUIRED XRootD_Cluster RUN_SERIAL 1 DISCOVERY_TIMEOUT 10)
endforeach()
