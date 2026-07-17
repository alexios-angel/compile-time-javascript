# the suite is static_asserts: compiling a test IS running it
add_custom_target(ctjs-test ALL)

file(GLOB test-sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# tests with a main() run; the rest are static_assert suites where
# compiling is the test
set(runtime-tests "")

foreach(source ${test-sources})
  get_filename_component(test "${source}" NAME_WE)
  if(test IN_LIST runtime-tests)
    add_executable(ctjs-test-${test} "${source}")
    target_link_libraries(ctjs-test-${test} PRIVATE ctjs::ctjs)
    if(CTJS_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
      target_precompile_headers(ctjs-test-${test} PRIVATE
        "${PROJECT_SOURCE_DIR}/include/ctjs.hpp")
    endif()
    add_dependencies(ctjs-test ctjs-test-${test})
    add_test(NAME run-${test} COMMAND ctjs-test-${test})
  else()
    add_library(ctjs-test-${test} OBJECT EXCLUDE_FROM_ALL "${source}")
    target_link_libraries(ctjs-test-${test} PRIVATE ctjs::ctjs)
    if(CTJS_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
      target_precompile_headers(ctjs-test-${test} PRIVATE
        "${PROJECT_SOURCE_DIR}/include/ctjs.hpp")
    endif()
    add_dependencies(ctjs-test ctjs-test-${test})
    add_test(NAME compile-${test}
      COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target ctjs-test-${test})
  endif()
endforeach()
