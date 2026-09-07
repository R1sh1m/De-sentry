cmake_minimum_required(VERSION 3.16)
project(desentry LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
  add_compile_options(/W3 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Optional vendored OSS backbone.
#
# Every one of these is OFF by default, and the tree builds and runs
# completely with all of them off -- that is the shipping configuration, and
# it is what makes "zero fetched dependencies at runtime" true rather than
# aspirational. Turning one on requires its amalgamation to be present under
# third_party/ (see third_party/README.md for exact vendoring steps and
# licences). Nothing here is downloaded by the build.
# ---------------------------------------------------------------------------
option(DESENTRY_WITH_SQLITE     "Build the SQLite backend (public domain)"        OFF)
option(DESENTRY_WITH_SQLITE_VEC "Build the sqlite-vec backend (Apache-2.0/MIT)"   OFF)
option(DESENTRY_WITH_DUCKDB     "Build the DuckDB backend (MIT)"                  OFF)
option(DESENTRY_WITH_LMDB       "Build the LMDB backend (OpenLDAP licence)"       OFF)
option(DESENTRY_BUILD_TESTS     "Build the test binaries"                         ON)

set(DESENTRY_VENDORED_SOURCES "")
set(DESENTRY_VENDORED_INCLUDES "")
set(DESENTRY_VENDORED_DEFINES "")

if(DESENTRY_WITH_SQLITE)
  set(_sqlite_dir "${CMAKE_SOURCE_DIR}/third_party/sqlite")
  if(NOT EXISTS "${_sqlite_dir}/sqlite3.c")
    message(FATAL_ERROR
      "DESENTRY_WITH_SQLITE=ON but ${_sqlite_dir}/sqlite3.c is missing. "
      "See third_party/README.md -- nothing is downloaded by this build.")
  endif()
  list(APPEND DESENTRY_VENDORED_SOURCES "${_sqlite_dir}/sqlite3.c")
  list(APPEND DESENTRY_VENDORED_INCLUDES "${_sqlite_dir}")
  # JSON1 and FTS5 are compile-time features of the amalgamation; the SQLite
  # backend degrades gracefully without FTS5 but there is no reason to build
  # without it.
  list(APPEND DESENTRY_VENDORED_DEFINES
    DESENTRY_WITH_SQLITE
    SQLITE_ENABLE_JSON1
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0)
endif()

if(DESENTRY_WITH_SQLITE_VEC)
  if(NOT DESENTRY_WITH_SQLITE)
    message(FATAL_ERROR "DESENTRY_WITH_SQLITE_VEC requires DESENTRY_WITH_SQLITE=ON")
  endif()
  set(_vec_dir "${CMAKE_SOURCE_DIR}/third_party/sqlite-vec")
  if(NOT EXISTS "${_vec_dir}/sqlite-vec.c")
    message(FATAL_ERROR
      "DESENTRY_WITH_SQLITE_VEC=ON but ${_vec_dir}/sqlite-vec.c is missing. See third_party/README.md.")
  endif()
  list(APPEND DESENTRY_VENDORED_SOURCES "${_vec_dir}/sqlite-vec.c")
  list(APPEND DESENTRY_VENDORED_INCLUDES "${_vec_dir}")
  list(APPEND DESENTRY_VENDORED_DEFINES DESENTRY_WITH_SQLITE_VEC SQLITE_CORE)
endif()

if(DESENTRY_WITH_DUCKDB)
  set(_duckdb_dir "${CMAKE_SOURCE_DIR}/third_party/duckdb")
  if(NOT EXISTS "${_duckdb_dir}/duckdb.hpp")
    message(FATAL_ERROR
      "DESENTRY_WITH_DUCKDB=ON but ${_duckdb_dir}/duckdb.hpp is missing. See third_party/README.md.")
  endif()
  # DuckDB's amalgamation is large enough that it is built as its own target
  # rather than folded into desentry_core, so an incremental engine change
  # does not trigger a multi-minute rebuild of it.
  add_library(desentry_duckdb STATIC "${_duckdb_dir}/duckdb.cpp")
  target_include_directories(desentry_duckdb PUBLIC "${_duckdb_dir}")
  list(APPEND DESENTRY_VENDORED_INCLUDES "${_duckdb_dir}")
  list(APPEND DESENTRY_VENDORED_DEFINES DESENTRY_WITH_DUCKDB)
endif()

if(DESENTRY_WITH_LMDB)
  set(_lmdb_dir "${CMAKE_SOURCE_DIR}/third_party/lmdb")
  if(NOT EXISTS "${_lmdb_dir}/mdb.c")
    message(FATAL_ERROR
      "DESENTRY_WITH_LMDB=ON but ${_lmdb_dir}/mdb.c is missing. See third_party/README.md.")
  endif()
  list(APPEND DESENTRY_VENDORED_SOURCES "${_lmdb_dir}/mdb.c" "${_lmdb_dir}/midl.c")
  list(APPEND DESENTRY_VENDORED_INCLUDES "${_lmdb_dir}")
  list(APPEND DESENTRY_VENDORED_DEFINES DESENTRY_WITH_LMDB)
endif()

# ---------------------------------------------------------------------------
# Core library: storage engine + router backends + CRDT layer + ledger +
# network layer + security + API server + supervisor. Everything the node
# daemon and the test binaries link against lives here so it is compiled
# exactly once.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DESENTRY_CORE_SRC CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/common/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/storage/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/crdt/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/ledger/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/net/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/security/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/api/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/engine/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/supervisor/*.cpp"
)

add_library(desentry_core STATIC ${DESENTRY_CORE_SRC} ${DESENTRY_VENDORED_SOURCES})
target_include_directories(desentry_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
if(DESENTRY_VENDORED_INCLUDES)
  target_include_directories(desentry_core PUBLIC ${DESENTRY_VENDORED_INCLUDES})
endif()
if(DESENTRY_VENDORED_DEFINES)
  target_compile_definitions(desentry_core PUBLIC ${DESENTRY_VENDORED_DEFINES})
endif()

target_link_libraries(desentry_core PUBLIC
  OpenSSL::Crypto
  OpenSSL::SSL
  Threads::Threads
)
if(DESENTRY_WITH_DUCKDB)
  target_link_libraries(desentry_core PUBLIC desentry_duckdb)
endif()

# Platform libraries. Winsock, the ACL API used by RestrictToOwner(), and the
# volume/drive enumeration in the hardware scanner all live in system DLLs
# that have to be named explicitly on Windows.
if(WIN32)
  target_link_libraries(desentry_core PUBLIC ws2_32 advapi32 mswsock)
elseif(APPLE)
  # getmntinfo lives in libSystem; nothing extra to link.
elseif(UNIX)
  target_link_libraries(desentry_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Node daemon: the actual peer binary. The desktop app ships this as a
# sidecar and never asks the user to run it directly.
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
# test framework, so the whole project builds offline with zero fetched
# dependencies).
# ---------------------------------------------------------------------------
if(DESENTRY_BUILD_TESTS)
  enable_testing()
  file(GLOB DESENTRY_TEST_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_src ${DESENTRY_TEST_SRC})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE desentry_core)
    # Tests rely on assert() for their actual checks. CMake's default
    # RelWithDebInfo/Release flags define NDEBUG, which compiles assert() into
    # a no-op -- silently turning every test into "PASS" regardless of what it
    # checked. -UNDEBUG (placed after the build type's -DNDEBUG on the compile
    # line) keeps assertions live in test binaries specifically, whatever
    # CMAKE_BUILD_TYPE is, without weakening optimisation for engine code.
    if(MSVC)
      target_compile_options(${test_name} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${test_name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Install rules. The desktop app's bundler (app/src-tauri/tauri.conf.json)
# picks the binaries up from here as versioned sidecars.
# ---------------------------------------------------------------------------
install(TARGETS desentryd desentry_cli RUNTIME DESTINATION bin)
install(FILES "${CMAKE_SOURCE_DIR}/config/node.example.json" DESTINATION share/desentry)

message(STATUS "de-sentry build configuration:")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests           : ${DESENTRY_BUILD_TESTS}")
message(STATUS "  sqlite backend  : ${DESENTRY_WITH_SQLITE}")
message(STATUS "  sqlite-vec      : ${DESENTRY_WITH_SQLITE_VEC}")
message(STATUS "  duckdb backend  : ${DESENTRY_WITH_DUCKDB}")
message(STATUS "  lmdb backend    : ${DESENTRY_WITH_LMDB}")
message(STATUS "  from-scratch engines (always built): kv, columnar_lite, ts_rollup, vector_hnsw_lite, graph_adj")
