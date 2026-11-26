cmake_minimum_required(VERSION 3.20)

project(hermes
  VERSION 0.1.0
  DESCRIPTION "Holistic Engine for Routability-driven Mixed-size placement with Embedded STA"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HERMES_BUILD_PYTHON "Build the Python bindings" ON)
option(HERMES_BUILD_TESTS  "Build the C++ tests"      ON)
option(HERMES_BUILD_APPS   "Build the hermes CLI"     ON)
option(HERMES_BUILD_BENCH  "Build the micro-benchmarks" ON)
# GUI viewer (hermes_gui): needs OpenGL 3.3 and GLFW 3.3+ (found on the
# system, e.g. `brew install glfw`, else downloaded). Dear ImGui is vendored
# in third_party/. The engine builds without it.
option(HERMES_BUILD_GUI    "Build the hermes_gui viewer" ON)
option(HERMES_GUI_FETCH_GLFW "Download GLFW if it is not installed" ON)
# Sanitizers for the tests: HERMES_SANITIZE=thread (data races) or
# address (memory errors). Applies to every target in the build.
set(HERMES_SANITIZE "" CACHE STRING "Sanitizer: thread | address | empty")
if(HERMES_SANITIZE)
  add_compile_options(-fsanitize=${HERMES_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${HERMES_SANITIZE})
endif()
# Highest log level compiled in (0=off .. 6=trace). Empty = Debug (5) when
# NDEBUG is defined (Release), Trace (6) otherwise. Calls above it vanish.
set(HERMES_LOG_MAX_LEVEL "" CACHE STRING "Highest compiled-in log level (0-6, empty = auto)")

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(hermes_core
  src/log/log.cpp
  src/log/sinks.cpp
  src/par/thread_pool.cpp
  src/par/job.cpp
  src/compute/cpu_backend.cpp
  src/db/design.cpp
  src/db/geometry.cpp
  src/db/id_list_pool.cpp
  src/db/name_table.cpp
  src/db/snapshot.cpp
  src/db/synthetic.cpp
  src/timing/sta.cpp
  src/place/congestion.cpp
  src/place/electrostatics.cpp
  src/place/placer.cpp
  src/place/legalizer.cpp
  src/route/global_router.cpp
  src/route/detailed_router.cpp
  src/route/route_usage.cpp
  src/route/route_shapes.cpp
  src/route/route_graph.cpp
  src/qor/qor.cpp
  src/track/run_tracker.cpp
  src/track/http_post.cpp
  src/flow/flow.cpp
  src/region/region.cpp
  src/io/common.cpp
  src/io/lefdef.cpp
  src/io/bookshelf.cpp
  src/io/codec.cpp
  src/io/design_file.cpp
  src/io/registry.cpp
)
add_library(hermes::core ALIAS hermes_core)

find_package(Threads REQUIRED)
target_link_libraries(hermes_core PUBLIC Threads::Threads)
target_include_directories(hermes_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                               ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Run tracking stores runs in SQLite (system library: macOS has it built in;
# Linux: libsqlite3-dev). Without it, runs can still be pushed to a server.
option(HERMES_WITH_SQLITE "Local run database (SQLite)" ON)
if(HERMES_WITH_SQLITE)
  find_package(SQLite3 QUIET)
endif()
if(SQLite3_FOUND)
  target_link_libraries(hermes_core PRIVATE SQLite::SQLite3)
  target_compile_definitions(hermes_core PRIVATE HERMES_HAVE_SQLITE=1)
else()
  message(STATUS "hermes: SQLite not found, building without the local run database")
  target_compile_definitions(hermes_core PRIVATE HERMES_HAVE_SQLITE=0)
endif()

# Build version string (git describe, or a hash of the sources), refreshed on every build.
add_custom_target(hermes_version ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/hermes_version.h
          -DVERSION=${PROJECT_VERSION} -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hermes_version.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/hermes_version.h
  COMMENT "hermes build version")
add_dependencies(hermes_core hermes_version)
if(NOT HERMES_LOG_MAX_LEVEL STREQUAL "")
  target_compile_definitions(hermes_core PUBLIC HERMES_LOG_MAX_LEVEL=${HERMES_LOG_MAX_LEVEL})
endif()

target_include_directories(hermes_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

if(MSVC)
  target_compile_options(hermes_core PRIVATE /W4)
else()
  target_compile_options(hermes_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(HERMES_BUILD_APPS)
  add_executable(hermes apps/hermes_main.cpp apps/assist_cli.cpp)
  target_include_directories(hermes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hermes PRIVATE hermes::core hermes_assist)
endif()

# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
# Scene model (snapshots -> drawable scene, picking, session/jobs, commands):
# no graphics dependencies, so it is built and tested everywhere.
if(HERMES_BUILD_GUI OR HERMES_BUILD_TESTS OR HERMES_BUILD_APPS OR HERMES_BUILD_PYTHON)
  add_library(hermes_gui_scene STATIC
    gui/scene/layers.cpp
    gui/scene/scene.cpp
    gui/scene/session.cpp
    gui/scene/worker.cpp
    gui/scene/commands.cpp)
  target_include_directories(hermes_gui_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hermes_gui_scene PUBLIC hermes::core)
  if(NOT MSVC)
    target_compile_options(hermes_gui_scene PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

# ---------------------------------------------------------------------------
# AI assistant integration (optional at run time; off unless configured)
# ---------------------------------------------------------------------------
# Tools over the session/command layer, an MCP server (hermes mcp), and the
# built-in chat with model providers (Anthropic, OpenAI-compatible incl.
# local Ollama/llama.cpp, mock). HTTPS through libcurl (macOS: built in;
# Linux: libcurl4-openssl-dev); without it only MCP and the mock provider work.
option(HERMES_WITH_CURL "HTTP client for the chat assistant (libcurl)" ON)
if(TARGET hermes_gui_scene)
  add_library(hermes_assist STATIC
    assist/json.cpp
    assist/tools.cpp
    assist/run_history.cpp
    assist/provider.cpp
    assist/http_curl.cpp
    assist/config.cpp
    assist/assistant.cpp
    assist/mcp_server.cpp
    assist/ai_host.cpp)
  target_include_directories(hermes_assist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hermes_assist PUBLIC hermes_gui_scene)
  if(HERMES_WITH_CURL)
    find_package(CURL QUIET)
  endif()
  if(CURL_FOUND)
    target_link_libraries(hermes_assist PRIVATE CURL::libcurl)
    target_compile_definitions(hermes_assist PRIVATE HERMES_HAVE_CURL=1)
  else()
    message(STATUS "hermes: libcurl not found, the chat assistant can only use the mock provider (MCP works)")
    target_compile_definitions(hermes_assist PRIVATE HERMES_HAVE_CURL=0)
  endif()
  if(SQLite3_FOUND)
    target_link_libraries(hermes_assist PRIVATE SQLite::SQLite3)
    target_compile_definitions(hermes_assist PRIVATE HERMES_HAVE_SQLITE=1)
  else()
    target_compile_definitions(hermes_assist PRIVATE HERMES_HAVE_SQLITE=0)
  endif()
  if(NOT MSVC)
    target_compile_options(hermes_assist PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
  endif()
endif()

if(HERMES_BUILD_GUI)
  if(APPLE)
    set(OpenGL_GL_PREFERENCE LEGACY)
  else()
    set(OpenGL_GL_PREFERENCE GLVND)
  endif()
  find_package(OpenGL QUIET)
  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND AND NOT TARGET glfw AND HERMES_GUI_FETCH_GLFW)
    message(STATUS "hermes: GLFW not installed, downloading GLFW 3.4")
    include(FetchContent)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
    FetchContent_MakeAvailable(glfw)
  endif()
  if(NOT OpenGL_FOUND OR NOT TARGET glfw)
    message(WARNING "hermes: OpenGL or GLFW not available, hermes_gui is not built "
                    "(install GLFW, e.g. `brew install glfw` / `apt install libglfw3-dev`)")
  else()
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_library(hermes_imgui STATIC
      ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
      ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
      ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(hermes_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(hermes_imgui PUBLIC glfw OpenGL::GL)
    target_compile_definitions(hermes_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    if(APPLE)
      target_compile_definitions(hermes_imgui PUBLIC GL_SILENCE_DEPRECATION)
    endif()

    # Fonts compiled into the binary (no files to find at startup).
    set(HERMES_GUI_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated/gui)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake)
    hermes_embed_file(${IMGUI_DIR}/fonts/Roboto-Medium.ttf ${HERMES_GUI_GEN}/font_ui.inc kFontUi)
    hermes_embed_file(${IMGUI_DIR}/fonts/Cousine-Regular.ttf ${HERMES_GUI_GEN}/font_mono.inc kFontMono)

    add_executable(hermes_gui
      gui/app/main.cpp
      gui/app/app.cpp
      gui/app/gl_renderer.cpp
      gui/app/ui.cpp)
    target_include_directories(hermes_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb ${HERMES_GUI_GEN})
    target_link_libraries(hermes_gui PRIVATE hermes_gui_scene hermes_assist hermes_imgui)
    if(NOT APPLE)
      target_compile_definitions(hermes_gui PRIVATE GL_GLEXT_PROTOTYPES)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Python bindings (pybind11)
# ---------------------------------------------------------------------------
if(HERMES_BUILD_PYTHON)
  find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
  find_package(pybind11 CONFIG QUIET)
  if(NOT pybind11_FOUND)
    include(FetchContent)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG v2.13.6)
    FetchContent_MakeAvailable(pybind11)
  endif()

  pybind11_add_module(_hermes python/bindings.cpp)
  target_link_libraries(_hermes PRIVATE hermes::core hermes_assist)

  if(SKBUILD)
    install(TARGETS _hermes LIBRARY DESTINATION hermes)
  else()
    # For in-tree development: place the extension next to the Python package.
    set_target_properties(_hermes PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python/hermes)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HERMES_BUILD_TESTS)
  enable_testing()
  add_executable(hermes_tests tests/test_core.cpp)
  target_link_libraries(hermes_tests PRIVATE hermes::core)
  add_test(NAME hermes_core_tests COMMAND hermes_tests)

  add_executable(hermes_lefdef_tests tests/test_lefdef.cpp)
  target_link_libraries(hermes_lefdef_tests PRIVATE hermes::core)
  target_compile_definitions(hermes_lefdef_tests PRIVATE
    HERMES_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME hermes_lefdef_tests COMMAND hermes_lefdef_tests)

  add_executable(hermes_io_tests tests/test_io.cpp)
  target_link_libraries(hermes_io_tests PRIVATE hermes::core)
  target_compile_definitions(hermes_io_tests PRIVATE
    HERMES_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME hermes_io_tests COMMAND hermes_io_tests)

  add_executable(hermes_db_tests tests/test_db.cpp)
  target_link_libraries(hermes_db_tests PRIVATE hermes::core)
  add_test(NAME hermes_db_tests COMMAND hermes_db_tests)

  add_executable(hermes_par_tests tests/test_par.cpp)
  target_link_libraries(hermes_par_tests PRIVATE hermes::core)
  add_test(NAME hermes_par_tests COMMAND hermes_par_tests)

  add_executable(hermes_qor_tests tests/test_qor.cpp)
  target_link_libraries(hermes_qor_tests PRIVATE hermes::core)
  target_compile_definitions(hermes_qor_tests PRIVATE
    HERMES_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  if(SQLite3_FOUND)
    target_link_libraries(hermes_qor_tests PRIVATE SQLite::SQLite3)
    target_compile_definitions(hermes_qor_tests PRIVATE HERMES_HAVE_SQLITE=1)
  endif()
  add_test(NAME hermes_qor_tests COMMAND hermes_qor_tests)

  add_executable(hermes_route_tests tests/test_route.cpp)
  target_link_libraries(hermes_route_tests PRIVATE hermes::core)
  add_test(NAME hermes_route_tests COMMAND hermes_route_tests)

  add_executable(hermes_gui_tests tests/test_gui.cpp)
  target_link_libraries(hermes_gui_tests PRIVATE hermes_gui_scene)
  add_test(NAME hermes_gui_tests COMMAND hermes_gui_tests)

  add_executable(hermes_assist_tests tests/test_assist.cpp)
  target_link_libraries(hermes_assist_tests PRIVATE hermes_assist)
  if(NOT MSVC)
    target_compile_options(hermes_assist_tests PRIVATE -Wno-missing-field-initializers)
  endif()
  if(SQLite3_FOUND)
    target_link_libraries(hermes_assist_tests PRIVATE SQLite::SQLite3)
    target_compile_definitions(hermes_assist_tests PRIVATE HERMES_HAVE_SQLITE=1)
  endif()
  add_test(NAME hermes_assist_tests COMMAND hermes_assist_tests)

  add_executable(hermes_log_tests tests/test_log.cpp)
  target_link_libraries(hermes_log_tests PRIVATE hermes::core)
  add_test(NAME hermes_log_tests COMMAND hermes_log_tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(HERMES_BUILD_BENCH)
  add_executable(hermes_log_bench bench/log_bench.cpp)
  target_link_libraries(hermes_log_bench PRIVATE hermes::core)
  add_executable(hermes_db_bench bench/db_bench.cpp)
  target_link_libraries(hermes_db_bench PRIVATE hermes::core)
  add_executable(hermes_par_bench bench/par_bench.cpp)
  target_link_libraries(hermes_par_bench PRIVATE hermes::core)
  add_executable(hermes_io_bench bench/io_bench.cpp)
  target_link_libraries(hermes_io_bench PRIVATE hermes::core)
  add_executable(hermes_gui_bench bench/gui_bench.cpp)
  target_link_libraries(hermes_gui_bench PRIVATE hermes_gui_scene)
  add_executable(hermes_qor_bench bench/qor_bench.cpp)
  target_link_libraries(hermes_qor_bench PRIVATE hermes::core)
  add_executable(hermes_route_bench bench/route_bench.cpp)
  target_link_libraries(hermes_route_bench PRIVATE hermes::core)
endif()
