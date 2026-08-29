cmake_minimum_required(VERSION 3.16)
project(desentry LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: storage engine + CRDT layer + network layer + security
# + API server. Everything the node daemon and the test binaries link
# against lives here so it is compiled exactly once.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DESENTRY_CORE_SRC CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/common/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/storage/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/crdt/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/net/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/security/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/api/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/engine/*.cpp"
)

add_library(desentry_core STATIC ${DESENTRY_CORE_SRC})
target_include_directories(desentry_core PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(desentry_core PUBLIC
  OpenSSL::Crypto
  OpenSSL::SSL
  Threads::Threads
)

# ---------------------------------------------------------------------------
# Node daemon: the actual peer binary.
# ---------------------------------------------------------------------------
add_executable(desentryd apps/desentry_node/main.cpp)
target_link_libraries(desentryd PRIVATE desentry_core)

# ---------------------------------------------------------------------------
# CLI helper: thin wrapper around the local HTTP API for demos/scripts.
# ---------------------------------------------------------------------------
add_executable(desentry_cli apps/desentry_cli/main.cpp)
target_link_libraries(desentry_cli PRIVATE desentry_core)

# ---------------------------------------------------------------------------
# Tests (simple, dependency-free assert-based test binaries -- no external
# test framework so the whole project builds offline with zero fetched
# dependencies).
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB DESENTRY_TEST_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*_test.cpp")
foreach(test_src ${DESENTRY_TEST_SRC})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE desentry_core)
  # Tests rely on assert() for their actual checks. CMake's default
  # RelWithDebInfo/Release flags define NDEBUG, which compiles assert()
  # into a no-op -- silently turning every test into "PASS" regardless of
  # what it checked. -UNDEBUG (placed after the build type's -DNDEBUG on
  # the compile line) keeps assertions live in test binaries specifically,
  # regardless of CMAKE_BUILD_TYPE, without weakening optimization flags
  # for the actual engine code in desentry_core.
  target_compile_options(${test_name} PRIVATE -UNDEBUG)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
