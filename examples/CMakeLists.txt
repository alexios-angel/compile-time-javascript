file(GLOB example-sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(source ${example-sources})
  get_filename_component(example "${source}" NAME_WE)
  add_executable(cthtml-example-${example} "${source}")
  target_link_libraries(cthtml-example-${example} PRIVATE cthtml::cthtml)
  if(CTHTML_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(cthtml-example-${example} PRIVATE
      "${PROJECT_SOURCE_DIR}/include/cthtml.hpp")
  endif()
  add_test(NAME example-${example} COMMAND cthtml-example-${example})
endforeach()
