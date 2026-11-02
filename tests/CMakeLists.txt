# One executable per area under test, each registered with add_test() so ctest
# can run them individually and report them separately.
#
# ndfex_test_case() takes the sources the test actually needs rather than
# linking all of ndfex_core. That keeps the link surface honest — a unit test
# for the config parser has no business dragging in sockets — and means an
# unrelated broken translation unit cannot stop these tests from running.
function(ndfex_test_case name)
    add_executable(${name} test_main.cpp ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE ndfex_warnings ndfex_sanitizers Threads::Threads)
    target_compile_definitions(${name} PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
    add_test(NAME ${name} COMMAND ${name})

    # Sanitizer defaults for every test: abort on the first error rather than
    # logging and continuing, so ctest actually sees a failure.
    #
    # detect_leaks is Linux-only — LeakSanitizer is not available in Apple's
    # ASan runtime, and setting the flag there just prints a warning on every
    # run. So leaks are caught in CI, not locally.
    set(_asan_opts "abort_on_error=1")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_asan_opts "${_asan_opts}:detect_leaks=1")
    endif()
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=${_asan_opts};UBSAN_OPTIONS=print_stacktrace=1"
    )
endfunction()

ndfex_test_case(test_config
    test_config.cpp
    ${CMAKE_SOURCE_DIR}/src/common/config.cpp
)
target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/src/common)

# C1 — position, average entry, and realized PnL in SymbolManager::on_fill.
ndfex_test_case(test_symbol_manager
    test_symbol_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/md/symbol_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/md/orderbook.cpp
)
target_include_directories(test_symbol_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/md
)

# P2 — snapshot parser against malformed packets. Pure function, no sockets.
ndfex_test_case(test_snapshot_parser
    test_snapshot_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/md/snapshot_parser.cpp
)
target_include_directories(test_snapshot_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/md
)
# A zero header.length used to spin forever. If that regresses, fail in
# seconds instead of hanging CI until the job-level timeout.
set_tests_properties(test_snapshot_parser PROPERTIES TIMEOUT 10)

# P2 (second half) — OE response framing over a real byte stream
# (socketpair), against malformed lengths, truncation, and EINTR.
ndfex_test_case(test_oe_framing
    test_oe_framing.cpp
    ${CMAKE_SOURCE_DIR}/src/oe/oe_framing.cpp
)
target_include_directories(test_oe_framing PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/oe
)
# Reading a body before validating its length blocks forever in one test.
set_tests_properties(test_oe_framing PROPERTIES TIMEOUT 10)

# P6 — market-data packets size-checked before any struct is read.
# Pure function over a byte buffer, no sockets.
ndfex_test_case(test_md_packet_validator
    test_md_packet_validator.cpp
    ${CMAKE_SOURCE_DIR}/src/md/md_packet_validator.cpp
)
target_include_directories(test_md_packet_validator PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/md
)
set_tests_properties(test_md_packet_validator PROPERTIES TIMEOUT 10)

# M4 — multicast socket setup reports failure instead of exit(1), and does
# not leak the fd. Uses real sockets, but no network setup or root.
ndfex_test_case(test_multicast_socket
    test_multicast_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/md/multicast_socket.cpp
)
target_include_directories(test_multicast_socket PRIVATE
    ${CMAKE_SOURCE_DIR}/src/md
)
set_tests_properties(test_multicast_socket PROPERTIES TIMEOUT 10)

# --- Tests still to be written (AUDIT.md §5 step 2) ------------------------
#
#   test_send_loop        P3 — short writes and a closed peer.
