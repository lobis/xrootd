if(TARGET xrd)
  foreach(test stat ls cat)
    add_test(NAME XrdApps::xrd::${test}
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/xrd-${test}.sh")
    set_tests_properties(XrdApps::xrd::${test} PROPERTIES
      ENVIRONMENT "XRD=$<TARGET_FILE:xrd>")
  endforeach()
endif()
