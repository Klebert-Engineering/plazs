# Reuse parent targets in MapViewer/mapget; never create a second SQLite or zserio runtime.
if(NOT COMMAND CPMAddPackage)
  set(_cpm "${CMAKE_BINARY_DIR}/cmake/CPM-0.40.2.cmake")
  file(DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake
    "${_cpm}"
    EXPECTED_HASH SHA256=c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d
    TLS_VERIFY ON)
  include("${_cpm}")
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  CPMAddPackage("gh:nlohmann/json@3.11.3")
endif()
if(NOT TARGET SQLite::SQLite3)
  CPMAddPackage(NAME sqlite-cmake GITHUB_REPOSITORY ndsev/sqlite-cmake VERSION 0.2.4
    OPTIONS "SQLITE_CMAKE_BUILD_EXAMPLES OFF")
  add_sqlite(BACKEND PUBLIC ENABLE_FTS5 ON)
endif()
if(NOT TARGET ndsmath::ndsmath)
  set(BUILD_SHARED_LIBS OFF)
  CPMAddPackage(NAME ndsmath GITHUB_REPOSITORY ndsev/ndslive-math
    GIT_TAG 009bba06356dfd01f98c1570a003e31180788811 SOURCE_SUBDIR cpp
    OPTIONS "NDSMATH_BUILD_TESTS OFF" "NDSMATH_INSTALL OFF")
endif()
if(TARGET ndsmath AND NOT TARGET ndsmath::ndsmath)
  add_library(ndsmath::ndsmath ALIAS ndsmath)
endif()
if(NOT TARGET zserio-cmake-helper)
  set(ZSERIO_VERSION 2.16.1)
  CPMAddPackage("gh:Klebert-Engineering/zserio-cmake-helper@1.1.4")
endif()
if(NOT TARGET ZserioCppRuntime)
  add_zserio_cpp_runtime()
endif()
