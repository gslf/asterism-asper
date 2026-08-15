cmake_minimum_required(VERSION 3.16)
project(asper C)

# ── Options ──────────────────────────────────────────────────────────────────
option(ASPER_BUILD_MCP   "Build the asper-mcp server"            ON)
option(ASPER_BUILD_TESTS "Build unit/integration tests"          ON)
option(ASPER_NO_THREADS  "Single-threaded build (asper_tick)"    OFF)
option(ASPER_SANITIZERS  "Enable ASan/UBSan"                     OFF)
option(ASPER_WITH_LLAMA  "Build llama.cpp backends"              ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Strict flags apply to Asper translation units only (deps use their own).
if(MSVC)
  set(ASPER_C_FLAGS /W4 /WX)
else()
  set(ASPER_C_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASPER_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────
# xCDN-C: official parser/serializer for all persistent text (D3). With
# tests enabled its own test/example targets build too (its add_test entries
# are registered either way, so they must be buildable to run).
# Guarded so an embedding host (e.g. asngn) that already provides the xcdn
# target can add this project via add_subdirectory without a collision.
if(NOT TARGET xcdn)
  if(ASPER_BUILD_TESTS)
    add_subdirectory(deps/xcdn-c)
  else()
    add_subdirectory(deps/xcdn-c EXCLUDE_FROM_ALL)
  endif()
endif()

# llama.cpp: curator inference + embeddings (consumed via its C API only).
if(ASPER_WITH_LLAMA)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/llama.cpp/CMakeLists.txt)
    message(FATAL_ERROR "deps/llama.cpp missing — run: git submodule update --init")
  endif()
  set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
  if(APPLE)
    # CPU-only baseline (§13); enable Metal explicitly if desired.
    set(GGML_METAL OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(deps/llama.cpp EXCLUDE_FROM_ALL)
endif()

# ── libasper ─────────────────────────────────────────────────────────────────
set(ASPER_SOURCES
  src/util.c
  src/time.c
  src/uuid.c
  src/sha256.c
  src/log.c
  src/config.c
  src/manifest.c
  src/store.c
  src/journal.c
  src/cache.c
  src/index.c
  src/decay.c
  src/retrieve.c
  src/inject.c
  src/grammar.c
  src/curator.c
  src/worker.c
  src/api.c
  src/embed_llama.c
  src/curator_llama.c
  src/os_common.c
)
if(WIN32)
  list(APPEND ASPER_SOURCES src/os_win32.c)
else()
  list(APPEND ASPER_SOURCES src/os_posix.c)
endif()

add_library(asper_obj OBJECT ${ASPER_SOURCES})
target_include_directories(asper_obj
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(asper_obj PRIVATE ${ASPER_C_FLAGS})
target_link_libraries(asper_obj PUBLIC xcdn)
if(ASPER_NO_THREADS)
  target_compile_definitions(asper_obj PUBLIC ASPER_NO_THREADS)
endif()
if(ASPER_WITH_LLAMA)
  target_compile_definitions(asper_obj PRIVATE ASPER_WITH_LLAMA)
  target_link_libraries(asper_obj PRIVATE llama)
endif()

add_library(asper_static STATIC $<TARGET_OBJECTS:asper_obj>)
set_target_properties(asper_static PROPERTIES OUTPUT_NAME asper)
add_library(asper_shared SHARED $<TARGET_OBJECTS:asper_obj>)
set_target_properties(asper_shared PROPERTIES OUTPUT_NAME asper)

foreach(tgt asper_static asper_shared)
  target_include_directories(${tgt} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(${tgt} PUBLIC xcdn)
  if(ASPER_WITH_LLAMA)
    target_link_libraries(${tgt} PUBLIC llama)
  endif()
  if(NOT WIN32 AND NOT ASPER_NO_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(${tgt} PUBLIC Threads::Threads)
  endif()
  if(NOT WIN32)
    target_link_libraries(${tgt} PUBLIC m)
  endif()
endforeach()

# ── asper-mcp ────────────────────────────────────────────────────────────────
if(ASPER_BUILD_MCP)
  add_executable(asper-mcp mcp/main.c mcp/json.c)
  target_include_directories(asper-mcp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp)
  target_compile_options(asper-mcp PRIVATE ${ASPER_C_FLAGS})
  target_link_libraries(asper-mcp PRIVATE asper_static)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ASPER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
