# Standalone from melonDS's own (huge) CMakeLists.txt on purpose -- see
# test_finlink_messages.cpp's own top comment. Points at the top-level
# finlink/core sibling checkout (not src/finlink/'s own stale, hand-synced
# vendored copy) so the round-trip assertions test against the current,
# correct protocol shape.
cmake_minimum_required(VERSION 3.13)
project(finlink_messages_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

if(NOT DEFINED FINLINK_CORE_DIR)
    set(FINLINK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../finlink/core")
endif()

add_executable(finlink_messages_test
    test_finlink_messages.cpp
    ../FinlinkMessages.cpp
    "${FINLINK_CORE_DIR}/src/json.c"
    "${FINLINK_CORE_DIR}/src/handshake.c"
)
target_include_directories(finlink_messages_test PRIVATE "${FINLINK_CORE_DIR}/include")

enable_testing()
add_test(NAME finlink_messages COMMAND finlink_messages_test)
