# the suite is static_asserts: compiling a test IS running it
add_custom_target(cthtml-test ALL)

file(GLOB test-sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# tests with a main() run; the rest are static_assert suites where
# compiling is the test
set(runtime-tests "")

foreach(source ${test-sources})
  get_filename_component(test "${source}" NAME_WE)
  if(test IN_LIST runtime-tests)
    add_executable(cthtml-test-${test} "${source}")
    target_link_libraries(cthtml-test-${test} PRIVATE cthtml::cthtml)
    if(CTHTML_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
      target_precompile_headers(cthtml-test-${test} PRIVATE
        "${PROJECT_SOURCE_DIR}/include/cthtml.hpp")
    endif()
    add_dependencies(cthtml-test cthtml-test-${test})
    add_test(NAME run-${test} COMMAND cthtml-test-${test})
  else()
    add_library(cthtml-test-${test} OBJECT EXCLUDE_FROM_ALL "${source}")
    target_link_libraries(cthtml-test-${test} PRIVATE cthtml::cthtml)
    if(CTHTML_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
      target_precompile_headers(cthtml-test-${test} PRIVATE
        "${PROJECT_SOURCE_DIR}/include/cthtml.hpp")
    endif()
    add_dependencies(cthtml-test cthtml-test-${test})
    add_test(NAME compile-${test}
      COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target cthtml-test-${test})
  endif()
endforeach()
