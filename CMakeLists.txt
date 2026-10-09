cmake_minimum_required(VERSION 3.22)

project(aoahid_share VERSION 0.3.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AOAHID_SHARE_BUILD_GUI "Build the settings window (aoahid_share_gui)" ON)
option(AOAHID_SHARE_WERROR "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Every executable lands in one directory, as in the release archives.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")

# An installed program finds the libaoahid library that sits next to it.
if(APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

function(aoahid_share_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
                                                 _CRT_SECURE_NO_WARNINGS)
    if(AOAHID_SHARE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion
                                             -Wsign-conversion)
    if(AOAHID_SHARE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# 4.0.0 is the first libaoahid with the Channel API the vendored
# aoahid_adb_proxy needs.
find_package(aoahid 4.0.0 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(src)
add_subdirectory(apps)

include(CTest)
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
  add_subdirectory(tests)
endif()
