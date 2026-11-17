cmake_minimum_required(VERSION 3.16)
project(edge-proxy CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Profiling builds: keep -O2 so you measure the code you ship, but always keep
# debug info and frame pointers so sample(1) / Instruments can walk stacks.
option(EDGE_PROXY_PROFILE "Build with -O2 -g -fno-omit-frame-pointer for profiling" OFF)
if(EDGE_PROXY_PROFILE)
  add_compile_options(-O2 -g -fno-omit-frame-pointer)
  message(STATUS "Profiling flags enabled (-O2 -g -fno-omit-frame-pointer)")
endif()

# Boost.Asio/Beast are header-only; only the headers are needed.
find_package(Boost QUIET CONFIG)
if(NOT Boost_FOUND)
  # e.g. Homebrew installs boost outside default search paths
  find_program(BREW brew)
  if(BREW)
    execute_process(COMMAND "${BREW}" --prefix boost
                    OUTPUT_VARIABLE BOOST_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND CMAKE_PREFIX_PATH "${BOOST_PREFIX}")
  endif()
  find_package(Boost REQUIRED CONFIG)
endif()

# The proxy
add_executable(edge-proxy-asio src/async_proxy.cpp)
target_compile_options(edge-proxy-asio PRIVATE -O2 -g -fno-omit-frame-pointer -Wall -Wextra)
target_link_libraries(edge-proxy-asio PRIVATE Boost::headers)
target_link_options(edge-proxy-asio PRIVATE -pthread)

# Test backend used by the benchmark scripts
add_executable(server src/server.cpp)
target_compile_options(server PRIVATE -O2 -Wall -Wextra)
target_link_libraries(server PRIVATE Boost::headers)
target_link_options(server PRIVATE -pthread)
