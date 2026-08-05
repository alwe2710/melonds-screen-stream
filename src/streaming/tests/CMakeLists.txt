# Standalone from melonDS's own (huge) CMakeLists.txt on purpose -- see
# test_unison_messages.cpp's own top comment. Points at the top-level
# unison/core sibling checkout (not src/unison/'s own stale, hand-synced
# vendored copy) so the round-trip assertions test against the current,
# correct protocol shape.
cmake_minimum_required(VERSION 3.13)
project(unison_messages_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

if(NOT DEFINED UNISON_CORE_DIR)
    set(UNISON_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Unison/core")
endif()

add_executable(unison_messages_test
    test_unison_messages.cpp
    ../UnisonMessages.cpp
    "${UNISON_CORE_DIR}/src/json.c"
    "${UNISON_CORE_DIR}/src/handshake.c"
)
target_include_directories(unison_messages_test PRIVATE "${UNISON_CORE_DIR}/include")

enable_testing()
add_test(NAME unison_messages COMMAND unison_messages_test)
