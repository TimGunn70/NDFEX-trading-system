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

# --- Tests still to be written (AUDIT.md §5 step 2) ------------------------
#
#   test_symbol_manager   C1 — short-position PnL. Add with:
#                             ndfex_test_case(test_symbol_manager
#                                 test_symbol_manager.cpp
#                                 ${CMAKE_SOURCE_DIR}/src/md/symbol_manager.cpp
#                                 ${CMAKE_SOURCE_DIR}/src/md/orderbook.cpp)
#
#   test_oe_framing       P2 — malformed wire data: length < sizeof(header),
#                              length > buffer, zero-length snapshot records.
#
#   test_send_loop        P3 — short writes and a closed peer.
