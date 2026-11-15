cmake_minimum_required(VERSION 3.20)
project(cachebench VERSION 1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(TRASE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/trase")
set(MIMALLOC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/mimalloc")
foreach(dependency "${TRASE_ROOT}/src/backend/BackendSVG.cpp" "${MIMALLOC_ROOT}/CMakeLists.txt")
  if(NOT EXISTS "${dependency}")
    message(FATAL_ERROR "Missing submodule sources. Run: git submodule update --init --recursive")
  endif()
endforeach()

# Static allocator, with explicit C++ overrides in allocator.cpp. Keep C-runtime
# allocation separate, and disable unused binaries, tests, profiling and ISA flags.
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_USE_CXX ON CACHE STRING "" FORCE)
set(MI_OPT_ARCH OFF CACHE STRING "" FORCE)
set(MI_PROFILE OFF CACHE STRING "" FORCE)
set(MI_STATS OFF CACHE STRING "" FORCE)
add_subdirectory(3rdparty/mimalloc EXCLUDE_FROM_ALL)
# Deliberately bypass upstream's graphics/CURL discovery and font enumeration.
# These two implementation files provide the complete primitive SVG API.
add_library(trase_svg STATIC
  "${TRASE_ROOT}/src/backend/BackendSVG.cpp"
  "${TRASE_ROOT}/src/util/Colors.cpp")
target_include_directories(trase_svg SYSTEM PUBLIC "${TRASE_ROOT}/src")

# Separate translation unit, without IPO: each pass remains an opaque call to
# the runner. This is the portable optimization barrier for streaming buffers.
add_library(cachebench_kernels STATIC src/kernels.cpp)
target_include_directories(cachebench_kernels PUBLIC src)
add_executable(cachebench
  src/main.cpp
  src/allocator.cpp
  src/config.cpp
  src/measure.cpp
  src/plot.cpp)
target_link_libraries(cachebench PRIVATE cachebench_kernels trase_svg mimalloc-static)
if(MINGW)
  # LLVM-MinGW defaults to libc++.dll, whose internal new/delete calls do not
  # use the executable's mimalloc overrides. Keep C++ allocations in one heap.
  target_link_options(cachebench PRIVATE -static)
endif()
foreach(target trase_svg mimalloc-static cachebench_kernels cachebench)
  set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
endforeach()
foreach(target cachebench_kernels cachebench)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Keep the explicit copy loop visible rather than recognizing libc memcpy.
  target_compile_options(cachebench_kernels PRIVATE -fno-tree-loop-distribute-patterns)
endif()

