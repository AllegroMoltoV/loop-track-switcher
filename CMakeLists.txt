cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# The dock needs the frontend API and Qt. CMakePresets.json turns both on as well.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp src/loop-track-source.c src/loop-track-source.h)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Stack protection for the plugin. MSVC enables /GS by default; it is stated here to make the setting explicit.
if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /GS)
else()
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -fstack-protector-strong)
endif()

# Scenario test that loads the plugin into libobs and checks the audio it outputs (tests/integration-test.cpp).
# The test is always built on Windows. On Linux, the CI workflow (audio-test.yaml) turns it on.
option(ENABLE_AUDIO_TESTS "Build the audio scenario test on Linux" OFF)
set(AUDIO_TEST_SCENARIOS scaffold-tone)
if(WIN32 OR ENABLE_AUDIO_TESTS)
  add_executable(audio-test tests/integration-test.cpp)
  target_compile_features(audio-test PRIVATE cxx_std_17)
  target_link_libraries(audio-test PRIVATE OBS::libobs)
  add_dependencies(audio-test ${CMAKE_PROJECT_NAME})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    target_link_libraries(audio-test PRIVATE X11::X11)
  endif()
endif()

if(WIN32)
  enable_testing()
  # Versions of the prebuilt dependencies in buildspec.json; the folder under .deps is named after it
  set(AUDIO_TEST_DEPS_VERSION "2025-07-11" CACHE STRING "Prebuilt obs-deps version in buildspec.json")
  foreach(_scenario IN LISTS AUDIO_TEST_SCENARIOS)
    add_test(
      NAME audio-${_scenario}
      COMMAND audio-test "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${CMAKE_CURRENT_SOURCE_DIR}/data" ${_scenario}
    )
    # libobs, obs-frontend-api, and Qt are loaded from .deps. The Qt DLLs are only in the qt6 folder.
    set_tests_properties(
      audio-${_scenario}
      PROPERTIES
        ENVIRONMENT_MODIFICATION
          "PATH=path_list_prepend:${CMAKE_CURRENT_SOURCE_DIR}/.deps/bin/64bit;PATH=path_list_prepend:${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-${AUDIO_TEST_DEPS_VERSION}-x64/bin;PATH=path_list_prepend:${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-qt6-${AUDIO_TEST_DEPS_VERSION}-x64/bin"
    )
  endforeach()
endif()
