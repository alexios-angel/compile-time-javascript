cmake_minimum_required(VERSION 3.14...4.3)

if(NOT CMAKE_EXPERIMENTAL_CXX_STD AND NOT CMAKE_CXX_MODULE_STD)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30" AND CMAKE_VERSION VERSION_LESS_EQUAL "4.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0" AND CMAKE_VERSION VERSION_LESS "4.0.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3" AND CMAKE_VERSION VERSION_LESS "4.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
        set(CMAKE_CXX_MODULE_STD 1)
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
        set(CMAKE_CXX_MODULE_STD 1)
    endif()
endif()

project(ctjs
  HOMEPAGE_URL "https://github.com/alexios-angel/compile-time-html"
  VERSION 0.1.0
  LANGUAGES CXX)
set(PROJECT_DESCRIPTION "Compile-time JavaScript: parsed while you compile, runs when you run - scripts as types, closures and all.")

include(CMakePackageConfigHelpers)
include(CMakeDependentOption)
include(GNUInstallDirs)
include(CTest)

find_program(CTJS_DPKG_BUILDPACKAGE_FOUND dpkg-buildpackage)
find_program(CTJS_RPMBUILD_FOUND rpmbuild)

cmake_dependent_option(CTJS_BUILD_TESTS "Build ctjs tests" ON
  "BUILD_TESTING;CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR" OFF)
cmake_dependent_option(CTJS_BUILD_EXAMPLES "Build ctjs examples" ON
  "CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR" OFF)
cmake_dependent_option(CTJS_BUILD_PACKAGE "Build ctjs packages" ON
  "CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR" OFF)
cmake_dependent_option(CTJS_BUILD_PACKAGE_DEB
  "Create DEB package (${PROJECT_NAME})" ON
  "CTJS_BUILD_PACKAGE;CTJS_DPKG_BUILDPACKAGE_FOUND" OFF)
cmake_dependent_option(CTJS_BUILD_PACKAGE_RPM
  "Create RPM package (${PROJECT_NAME})" ON
  "CTJS_BUILD_PACKAGE;CTJS_RPMBUILD_FOUND" OFF)

option(CTJS_MODULE "build C++ module (experimental)" OFF)
# the value parser folds whole scripts in the constant evaluator; the
# interface carries the raised limits so consumers do not have to know
# the flags
option(CTJS_CONSTEXPR_LIMITS "Raise constexpr evaluation limits on the interface" ON)
option(CTJS_PCH "Precompile ctjs.hpp for tests and examples" ON)

set(CTJS_CONSTEXPR_OPTIONS
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-fconstexpr-steps=500000000;-fconstexpr-depth=1024;-fbracket-depth=2048>
  $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=3000000000;-fconstexpr-loop-limit=10000000;-fconstexpr-depth=1024>
  $<$<CXX_COMPILER_ID:MSVC>:/constexpr:steps500000000;/constexpr:depth1024>)

# ctlark and ctll live in a git submodule (git submodule update --init).
# The extra .../include/ctlark and .../include/ctll entries let the headers'
# relative `"../ctlark.hpp"`-style quoted includes resolve through the
# quoted-include fallback; the install keeps the flat include/{ctjs,ctlark,ctll}
# layout, so installed consumers need neither.
set(CTJS_SUBMODULE_INCLUDE_DIRS
  "${PROJECT_SOURCE_DIR}/external/compile-time-lark/include"
  "${PROJECT_SOURCE_DIR}/external/compile-time-lark/include/ctlark"
  "${PROJECT_SOURCE_DIR}/external/compile-time-lark/include/ctll")
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/compile-time-lark/include/ctlark.hpp")
  message(FATAL_ERROR
    "ctjs: the compile-time-lark submodule is missing - run `git submodule update --init`")
endif()

set(CTJS_HEADERS
	include/ctjs.hpp
	include/ctjs/builtins.hpp
	include/ctjs/cfunction.hpp
	include/ctjs/gc.hpp
	include/ctjs/rc.hpp
	include/ctjs/script.hpp
	include/ctjs/value.hpp
	include/ctjs/vinterp.hpp
	include/ctjs/vparse.hpp)
file(GLOB_RECURSE CTJS_CTLARK_HEADERS CONFIGURE_DEPENDS
	external/compile-time-lark/include/*.hpp)

if(CTJS_MODULE)
	if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.29.20240416")
		add_library(${PROJECT_NAME})

		target_sources(${PROJECT_NAME} PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES FILES ctjs.cppm)
		target_sources(${PROJECT_NAME} PUBLIC FILE_SET HEADERS TYPE HEADERS
		    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
		        "${CMAKE_CURRENT_SOURCE_DIR}/external/compile-time-lark/include"
		    FILES ${CTJS_HEADERS} ${CTJS_CTLARK_HEADERS})
		target_include_directories(${PROJECT_NAME} PUBLIC
		    "$<BUILD_INTERFACE:${CTJS_SUBMODULE_INCLUDE_DIRS}>")

		# we are using `import std;`
		if (NOT DEFINED CTJS_CXX_STANDARD OR CTJS_CXX_STANDARD LESS 23)
			set(CTJS_CXX_STANDARD 23)
		endif()

		target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_${CTJS_CXX_STANDARD})
		if(CTJS_CONSTEXPR_LIMITS)
			target_compile_options(${PROJECT_NAME} PUBLIC ${CTJS_CONSTEXPR_OPTIONS})
		endif()

		install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}-targets
			FILE_SET CXX_MODULES DESTINATION "${CMAKE_INSTALL_LIBDIR}/cxx/${PROJECT_NAME}"
			FILE_SET HEADERS DESTINATION "include")
	else()
		message(FATAL_ERROR "unsupported cmake for c++ modules")
	endif()
else()
	add_library(${PROJECT_NAME} INTERFACE)

	target_include_directories(${PROJECT_NAME} INTERFACE
	  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	  "$<BUILD_INTERFACE:${CTJS_SUBMODULE_INCLUDE_DIRS}>"
	  $<INSTALL_INTERFACE:include>)

	if (NOT CTJS_CXX_STANDARD)
	  set(CTJS_CXX_STANDARD 23)
	endif()

	target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_${CTJS_CXX_STANDARD})
	if(CTJS_CONSTEXPR_LIMITS)
		target_compile_options(${PROJECT_NAME} INTERFACE ${CTJS_CONSTEXPR_OPTIONS})
	endif()
	set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_SCAN_FOR_MODULES 0)

	install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}-targets)
	install(DIRECTORY include/ DESTINATION include
	    FILES_MATCHING PATTERN *.hpp)
	# the installed tree stays flat - include/{ctjs,ctlark,ctll} side by side -
	# so the headers' relative includes keep working for installed consumers
	install(DIRECTORY external/compile-time-lark/include/ DESTINATION include
	    FILES_MATCHING PATTERN *.hpp)
endif()

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

if (NOT EXISTS "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake.in")
  file(WRITE ${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake.in [[
    @PACKAGE_INIT@
    include("${CMAKE_CURRENT_LIST_DIR}/@PROJECT_NAME@-targets.cmake")
  ]])
endif()

configure_package_config_file(
  "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake.in"
  "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
  NO_SET_AND_CHECK_MACRO
  NO_CHECK_REQUIRED_COMPONENTS_MACRO)

write_basic_package_version_file(ctjs-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(EXPORT ${PROJECT_NAME}-targets
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
  NAMESPACE ${PROJECT_NAME}::)
install(
  FILES
    "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake"
    "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

# One shared PCH for every test and example target - compiles in
# seconds now (the grammar bake died with the type path); the anchor
# target owns the PCH and everything else REUSE_FROMs it.
if(CTJS_PCH AND (CTJS_BUILD_TESTS OR CTJS_BUILD_EXAMPLES)
   AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
  file(WRITE "${CMAKE_BINARY_DIR}/ctjs-pch-anchor.cpp"
    "int ctjs_pch_anchor_symbol = 0;\n")
  add_library(ctjs-pch-anchor OBJECT "${CMAKE_BINARY_DIR}/ctjs-pch-anchor.cpp")
  target_link_libraries(ctjs-pch-anchor PRIVATE ctjs::ctjs)
  target_precompile_headers(ctjs-pch-anchor PRIVATE
    "${PROJECT_SOURCE_DIR}/include/ctjs.hpp")
endif()

if(CTJS_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(CTJS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if (NOT CTJS_BUILD_PACKAGE)
  return()
endif()

list(APPEND source-generators TBZ2 TGZ TXZ ZIP)

if (CTJS_BUILD_PACKAGE_DEB)
  list(APPEND binary-generators "DEB")
endif()

if (CTJS_BUILD_PACKAGE_RPM)
  list(APPEND binary-generators "RPM")
endif()

set(CPACK_SOURCE_GENERATOR ${source-generators})
set(CPACK_GENERATOR ${binary-generators})

set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alexios Angel")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${PROJECT_DESCRIPTION}")
set(CPACK_DEBIAN_PACKAGE_NAME "lib${PROJECT_NAME}-dev")

set(CPACK_RPM_PACKAGE_NAME "lib${PROJECT_NAME}-devel")

set(PKG_CONFIG_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/pkgconfig.pc.in" "${PKG_CONFIG_FILE_NAME}" @ONLY)
install(FILES "${PKG_CONFIG_FILE_NAME}"
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

list(APPEND CPACK_SOURCE_IGNORE_FILES /.git/ /build/ .gitignore .DS_Store)

include(CPack)
