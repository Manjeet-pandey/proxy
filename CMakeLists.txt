cmake_minimum_required(VERSION 3.16)
project(edge-proxy CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Boost.Asio (header-only) for the async/event-loop proxy
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

add_executable(edge-proxy src/main.cpp)
target_compile_options(edge-proxy PRIVATE -O2 -Wall -Wextra)
target_link_options(edge-proxy PRIVATE -pthread)

# Test backend (Boost.Asio coroutines, multi-threaded, HTTP/1.1 keep-alive):
add_executable(server src/server.cpp)
target_compile_options(server PRIVATE -O2 -Wall -Wextra)
target_link_libraries(server PRIVATE Boost::headers)
target_link_options(server PRIVATE -pthread)

add_executable(spammer src/spammer.cpp)
target_compile_options(spammer PRIVATE -O2 -Wall -Wextra)
target_link_options(spammer PRIVATE -pthread)

add_executable(edge-proxy-asio src/async_proxy.cpp)
target_compile_options(edge-proxy-asio PRIVATE -O2 -g -fno-omit-frame-pointer -Wall -Wextra)
target_link_libraries(edge-proxy-asio PRIVATE Boost::headers)
target_link_options(edge-proxy-asio PRIVATE -pthread)
