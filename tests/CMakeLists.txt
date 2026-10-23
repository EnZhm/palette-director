# Self-contained offline Director test project. Builds the pure-C++ Director library
# (no libobs/Qt) plus a doctest suite driven by synthetic WAV fixtures.
#
# Standalone:  cmake -S tests -B build-tests && cmake --build build-tests \
#              && ctest --test-dir build-tests --output-on-failure
#
# It can also be pulled in from the top-level project with `add_subdirectory(tests)`
# behind the PD_BUILD_TESTS option (see the guarded block in the top-level CMakeLists.txt).
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pd_director_tests C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

enable_testing()

set(PD_DIRECTOR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/director)

add_library(
  pd_director
  STATIC
  ${PD_DIRECTOR_ROOT}/director.cpp
  ${PD_DIRECTOR_ROOT}/analysis.cpp
  ${PD_DIRECTOR_ROOT}/beat_tracker.cpp
  ${PD_DIRECTOR_ROOT}/section_state.cpp
  ${PD_DIRECTOR_ROOT}/scheduler.cpp
)
target_include_directories(pd_director PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Pure-C palette set model (libobs/Qt-free) exercised by test_palette.cpp.
set(PD_PALETTE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/palette)

add_library(pd_palette STATIC ${PD_PALETTE_ROOT}/palette.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/sets/set-file.cpp)
target_include_directories(pd_palette PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(
  pd_director_tests
  doctest_main.cpp
  test_rng.cpp
  test_bpm.cpp
  test_sections.cpp
  test_scheduler.cpp
  test_commands.cpp
  test_silence.cpp
  test_determinism.cpp
  test_palette.cpp
  test_cbuffer_layout.cpp
  test_breakdown_bpm.cpp
  test_host_api.cpp
  test_set_file.cpp
  test_audio_level.cpp
  fixtures/gen_fixtures.cpp
)
target_link_libraries(pd_director_tests PRIVATE pd_director pd_palette)
target_compile_definitions(pd_director_tests PRIVATE PD_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(
  pd_director_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/director
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest
)

add_test(NAME director_tests COMMAND pd_director_tests)
