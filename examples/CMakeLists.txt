file(GLOB example-sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(source ${example-sources})
  get_filename_component(example "${source}" NAME_WE)
  add_executable(ctjs-example-${example} "${source}")
  target_link_libraries(ctjs-example-${example} PRIVATE ctjs::ctjs)
  if(CTJS_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(ctjs-example-${example} PRIVATE
      "${PROJECT_SOURCE_DIR}/include/ctjs.hpp")
  endif()
  add_test(NAME example-${example} COMMAND ctjs-example-${example})
endforeach()
