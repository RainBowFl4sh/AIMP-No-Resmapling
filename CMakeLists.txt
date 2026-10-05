cmake_minimum_required(VERSION 3.16)
project(PreventResampling VERSION 2.5.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# AIMP SDK: either point to an existing "Sources/Cpp" folder or download it automatically.
set(AIMP_SDK_DIR "" CACHE PATH "Path to the SDK folder Sources/Cpp (contains apiPlugin.h). Empty = sdk/ in this repository")
set(AIMP_SDK_GIT "https://github.com/Exle/aimp-sdk.git" CACHE STRING "Git mirror of the AIMP SDK")
set(AIMP_SDK_TAG "beta-v6.00-Beta-6-9a31b5ce27a2" CACHE STRING "Tag/branch of the AIMP SDK (v6.00 build 3083)")

# The SDK headers are part of this repository (sdk/, AIMP 6.00 Beta 7). The download is only a fallback.
if(NOT AIMP_SDK_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/apiPlugin.h")
  set(AIMP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
endif()
if(NOT AIMP_SDK_DIR)
  include(FetchContent)
  FetchContent_Declare(aimp_sdk
    GIT_REPOSITORY ${AIMP_SDK_GIT}
    GIT_TAG ${AIMP_SDK_TAG}
    GIT_SHALLOW TRUE)
  message(STATUS "AIMP SDK: ${AIMP_SDK_GIT} (${AIMP_SDK_TAG})")
  FetchContent_MakeAvailable(aimp_sdk)  # the SDK has no CMakeLists.txt -> download only
  set(AIMP_SDK_DIR "${aimp_sdk_SOURCE_DIR}/Sources/Cpp")
endif()
if(NOT EXISTS "${AIMP_SDK_DIR}/apiPlugin.h")
  message(FATAL_ERROR "apiPlugin.h not found in '${AIMP_SDK_DIR}'. AIMP_SDK_DIR must point to the SDK's 'Sources/Cpp' folder.")
endif()
if(NOT EXISTS "${AIMP_SDK_DIR}/apiTypes.cpp")
  message(FATAL_ERROR "apiTypes.cpp is missing - AIMP SDK v6.00 or newer is required.")
endif()

# ---------------------------------------------------------------------------
# CHANGELOG.md is shown on the About tab, followed by res/changelog-history.md (the versions before
# 2.0, which are not part of CHANGELOG.md). Both are embedded as raw string literals, split at line
# ends into pieces of at most ~8 KB - MSVC rejects a single string literal longer than 16 KB.
set(AR_CHANGELOG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md" "${CMAKE_CURRENT_SOURCE_DIR}/res/changelog-history.md")
set(AR_CHANGELOG_LITERALS "")
foreach(f ${AR_CHANGELOG_FILES})
  file(READ "${f}" txt)
  string(APPEND txt "\n")
  string(LENGTH "${txt}" len)
  set(pos 0)
  while(pos LESS len)
    math(EXPR rest "${len} - ${pos}")
    set(take ${rest})
    if(rest GREATER 8000)
      string(SUBSTRING "${txt}" ${pos} 8000 piece)
      string(FIND "${piece}" "\n" nl REVERSE)
      if(nl LESS 0)
        set(nl 7999)
      endif()
      math(EXPR take "${nl} + 1")
    endif()
    string(SUBSTRING "${txt}" ${pos} ${take} piece)
    string(APPEND AR_CHANGELOG_LITERALS "R\"AR_CL(${piece})AR_CL\" ")
    math(EXPR pos "${pos} + ${take}")
  endwhile()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/generated/ChangelogText.h.tmp"
  "// Generated from CHANGELOG.md and res/changelog-history.md by CMake - do not edit\n#pragma once\n#define AR_CHANGELOG_TEXT ${AR_CHANGELOG_LITERALS}\n")
configure_file("${CMAKE_BINARY_DIR}/generated/ChangelogText.h.tmp" "${CMAKE_BINARY_DIR}/generated/ChangelogText.h" COPYONLY)

add_library(PreventResampling SHARED src/Plugin.cpp "${AIMP_SDK_DIR}/apiTypes.cpp")
target_include_directories(PreventResampling SYSTEM PRIVATE "${AIMP_SDK_DIR}")
target_include_directories(PreventResampling PRIVATE src "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(PreventResampling PRIVATE AR_VERSION="${PROJECT_VERSION}")
set_target_properties(PreventResampling PROPERTIES
  OUTPUT_NAME "PreventResampling"
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  target_compile_definitions(PreventResampling PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(PreventResampling PRIVATE ole32 oleaut32 uuid advapi32 user32 gdi32 shell32)
  if(MSVC)
    # Static runtime: users do not need the VC++ redistributable
    set_property(TARGET PreventResampling PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(PreventResampling PRIVATE /utf-8 /W3 /permissive-)
    # The .def file keeps the export names undecorated on 32 bit as well (__stdcall)
    target_sources(PreventResampling PRIVATE src/PreventResampling.def)
  else()
    # For non-MSVC compilers the SDK uses "wtoll"; MinGW provides "_wtoi64" instead
    set_source_files_properties("${AIMP_SDK_DIR}/apiTypes.cpp" PROPERTIES COMPILE_DEFINITIONS "wtoll=_wtoi64")
    # MinGW: link everything statically (no libstdc++/libgcc/winpthread DLLs needed)
    target_compile_options(PreventResampling PRIVATE -Wall -Wno-unknown-pragmas -Wno-attributes)
    target_link_options(PreventResampling PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at -s)
  endif()
else()
  # Linux: the SDK includes <cairo.h> (headers only, no library needed)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAIRO REQUIRED cairo)
  find_package(Threads REQUIRED)
  target_include_directories(PreventResampling PRIVATE ${CAIRO_INCLUDE_DIRS})
  target_compile_options(PreventResampling PRIVATE -Wall -Wno-unknown-pragmas -Wno-attributes)
  target_link_libraries(PreventResampling PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(PreventResampling PRIVATE -Wl,--no-undefined -Wl,-z,defs -s)
endif()

# Folder layout according to the AIMP SDK ("Plugins folder structure"):
#   PreventResampling/PreventResampling.dll       Windows 32 bit
#   PreventResampling/x64/PreventResampling.dll   Windows 64 bit
#   PreventResampling/x64/PreventResampling.so    Linux 64 bit
# "cmake --install build --prefix <AIMP>/Plugins" puts the file in the right place.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(AR_PLUGIN_SUBDIR "PreventResampling/x64")
else()
  set(AR_PLUGIN_SUBDIR "PreventResampling")
endif()
install(TARGETS PreventResampling
  RUNTIME DESTINATION "${AR_PLUGIN_SUBDIR}"
  LIBRARY DESTINATION "${AR_PLUGIN_SUBDIR}")

# ---------------------------------------------------------------------------
# Test without AIMP (Linux only): a mock host loads the plugin and "plays" tracks.
option(AR_BUILD_TESTS "Build the mock host test (Linux)" OFF)
if(AR_BUILD_TESTS AND UNIX)
  enable_testing()
  add_executable(MockHost tests/MockHost.cpp "${AIMP_SDK_DIR}/apiTypes.cpp")
  target_include_directories(MockHost SYSTEM PRIVATE "${AIMP_SDK_DIR}" ${CAIRO_INCLUDE_DIRS})
  target_link_libraries(MockHost PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  set(AR_TEST_DIR "${CMAKE_BINARY_DIR}/testrun")
  file(MAKE_DIRECTORY "${AR_TEST_DIR}/bin")
  file(COPY tests/pw-metadata DESTINATION "${AR_TEST_DIR}/bin"
       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  add_test(NAME MockHost
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${AR_TEST_DIR}/bin:$ENV{PATH}" "AR_PW_LOG=${AR_TEST_DIR}/pw.log"
            $<TARGET_FILE:MockHost> $<TARGET_FILE:PreventResampling> "${AR_TEST_DIR}")
endif()
