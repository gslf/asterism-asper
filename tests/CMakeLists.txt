# tests/CMakeLists.txt — one CTest executable per test_*.c.
#
# The strict-flag list mirrors the top-level ASPER_C_FLAGS convention; it is
# recreated here because the parent-scope variable may not propagate.
if(MSVC)
  set(ASPER_C_FLAGS /W4 /WX)
else()
  set(ASPER_C_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

# Mirrors the top-level ASPER_C_DEFS: feature-test macros for POSIX APIs
# (pthread_rwlock_t, etc.) pulled in via asper_internal.h/os.h. Must be a
# compiler define, not a header #define — see the top-level CMakeLists.txt.
if(WIN32 OR MSVC)
  set(ASPER_C_DEFS)
else()
  set(ASPER_C_DEFS _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
  if(APPLE)
    list(APPEND ASPER_C_DEFS _DARWIN_C_SOURCE)
  endif()
endif()

set(ASPER_TEST_PROGRAMS
  test_util
  test_time
  test_sha_uuid
  test_store
  test_journal
  test_decay_scoring
  test_index
  test_inject_golden
  test_curator_cycles
  test_review_recall
  test_config
  test_json
  test_log
  test_stats_projects
)

foreach(test_name IN LISTS ASPER_TEST_PROGRAMS)
  set(test_sources ${test_name}.c fakes.c)
  if(test_name STREQUAL "test_json")
    # The MCP JSON codec is not part of libasper; compile it into this test.
    list(APPEND test_sources ${CMAKE_CURRENT_SOURCE_DIR}/../mcp/json.c)
  endif()

  add_executable(${test_name} ${test_sources})
  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/xcdn-c/src
    ${CMAKE_CURRENT_SOURCE_DIR})
  if(test_name STREQUAL "test_json")
    target_include_directories(${test_name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../mcp)
  endif()
  set_target_properties(${test_name} PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  target_compile_options(${test_name} PRIVATE ${ASPER_C_FLAGS})
  target_compile_definitions(${test_name} PRIVATE ${ASPER_C_DEFS})
  target_link_libraries(${test_name} PRIVATE asper_static)

  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
