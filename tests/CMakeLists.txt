# every test is an executable: scripts parse at COMPILE time inside
# each TU (that part is "compiling is the test"), then the binary runs
# its checks - a non-zero exit fails ctest
add_custom_target(ctjs-test ALL)

file(GLOB test-sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(source ${test-sources})
  get_filename_component(test "${source}" NAME_WE)
  add_executable(ctjs-test-${test} "${source}")
  target_link_libraries(ctjs-test-${test} PRIVATE ctjs::ctjs)
  target_compile_options(ctjs-test-${test} PRIVATE ${CTJS_WARNING_OPTIONS})
  if(TARGET ctjs-pch-anchor)
    target_precompile_headers(ctjs-test-${test} REUSE_FROM ctjs-pch-anchor)
  endif()
  add_dependencies(ctjs-test ctjs-test-${test})
  add_test(NAME run-${test} COMMAND ctjs-test-${test})
endforeach()
