cmake_minimum_required(VERSION 3.20)
project(plazs VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(_plazs_top OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_plazs_top ON)
endif()
option(PLAZS_BUILD_TESTS "Build native and ingestion tests" ${_plazs_top})
option(PLAZS_BUILD_TOOLS "Build the native query tool" ${_plazs_top})

include(cmake/dependencies.cmake)

# Keep compiler/runtime code private. Python generation is for offline ingestion only.
set(PLAZS_PYTHON_DIR "${CMAKE_CURRENT_BINARY_DIR}/python" CACHE INTERNAL "Generated plazs schema")
add_zserio_library(plazs-schema WITHOUT_SQL WITH_POLYMORPHIC_ALLOC CACHE_GENERATION
  ROOT "${CMAKE_CURRENT_SOURCE_DIR}/schema" ENTRY plazs_data.zs
  EXTRA_ARGS -python "${PLAZS_PYTHON_DIR}" -withoutServiceCode -withoutPubsubCode)

add_library(plazs STATIC src/gazetteer.cpp)
add_library(plazs::plazs ALIAS plazs)
target_include_directories(plazs PUBLIC include)
target_link_libraries(plazs PUBLIC nlohmann_json::nlohmann_json
  PRIVATE SQLite::SQLite3 ndsmath::ndsmath plazs-schema)
target_compile_features(plazs PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(plazs PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(PLAZS_BUILD_TOOLS)
  add_executable(plazs-query tools/query.cpp)
  target_link_libraries(plazs-query PRIVATE plazs)
endif()

if(PLAZS_BUILD_TESTS)
  enable_testing()
  if(NOT TARGET Catch2::Catch2WithMain)
    CPMAddPackage("gh:catchorg/Catch2@3.5.2")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_package(Threads REQUIRED)
  add_executable(plazs-tests tests/test_gazetteer.cpp)
  target_compile_definitions(plazs-tests PRIVATE
    PLAZS_TEST_DATABASE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/places.sqlite")
  target_link_libraries(plazs-tests PRIVATE plazs plazs-schema SQLite::SQLite3
    ndsmath::ndsmath Catch2::Catch2WithMain Threads::Threads)
  add_test(NAME plazs-native COMMAND plazs-tests)
  # Set paths inside the test runner so Windows and Unix share the same invocation.
  add_test(NAME plazs-import COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.py" "${PLAZS_PYTHON_DIR}")
endif()
