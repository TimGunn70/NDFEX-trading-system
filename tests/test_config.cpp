#include "test_main.h"

#include "config.h"

#include <cstdlib>
#include <string>

#ifndef FIXTURE_DIR
#error "FIXTURE_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace {

std::string fixture(const char* name) {
    return std::string(FIXTURE_DIR) + "/" + name;
}

// Every accept-case needs the secret present, and every test sets it explicitly
// rather than relying on a shared setup step running first.
void set_password(const char* value) {
    if (value == nullptr) {
        ::unsetenv("NDFEX_PASSWORD");
    } else {
        ::setenv("NDFEX_PASSWORD", value, 1);
    }
}

std::optional<Config> load(const char* fixture_name, std::string& error) {
    return load_config(fixture(fixture_name), error);
}

// Asserts the loader rejects a fixture, and that it said something useful about
// why. An empty error string on failure is itself a bug: the caller has nothing
// to print.
void expect_rejected(const char* fixture_name) {
    set_password("secret");
    std::string error;
    const auto  cfg = load(fixture_name, error);
    if (cfg.has_value()) {
        ::testing::record_failure(__FILE__, __LINE__,
                                  std::string("expected rejection of fixture '")
                                      + fixture_name + "' but it loaded");
        return;
    }
    if (error.empty()) {
        ::testing::record_failure(__FILE__, __LINE__,
                                  std::string("fixture '") + fixture_name
                                      + "' was rejected with an empty error message");
    }
}

bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// --- the happy path -------------------------------------------------------

TEST(good_config_loads_every_field) {
    set_password("hunter2");
    std::string error;
    const auto  cfg = load("good.toml", error);

    EXPECT_TRUE(cfg.has_value());
    EXPECT_EQ(error, std::string());
    if (!cfg) return;

    EXPECT_EQ(cfg->exchange.host, std::string("10.0.0.1"));
    EXPECT_EQ(cfg->exchange.port, 1234);
    EXPECT_EQ(cfg->exchange.client_id, 8u);

    EXPECT_EQ(cfg->auth.team_name, std::string("group8"));
    EXPECT_EQ(cfg->auth.password, std::string("hunter2"));

    EXPECT_EQ(cfg->etf_service.url, std::string("http://10.0.0.2:5000"));

    EXPECT_EQ(cfg->market_data.live_addr,   std::string("239.0.0.1"));
    EXPECT_EQ(cfg->market_data.replay_addr, std::string("239.0.0.2"));
    EXPECT_EQ(cfg->market_data.local_iface, std::string("10.0.0.3"));
    EXPECT_EQ(cfg->market_data.port, 12345);

    EXPECT_EQ(cfg->risk.position_limit, 10);
    EXPECT_EQ(cfg->risk.mm_position_limit, 4);
    EXPECT_NEAR(cfg->risk.pnl_floor, -5000.0, 1e-9);
}

TEST(trailing_comments_are_stripped_but_not_inside_strings) {
    set_password("hunter2");
    std::string error;
    const auto  cfg = load("inline_comment.toml", error);

    EXPECT_TRUE(cfg.has_value());
    if (!cfg) return;

    EXPECT_EQ(cfg->exchange.port, 1234);
    // The '#' here is part of the URL, not the start of a comment.
    EXPECT_EQ(cfg->etf_service.url, std::string("http://10.0.0.2:5000/#frag"));
}

TEST(crlf_line_endings_are_accepted) {
    set_password("hunter2");
    std::string error;
    const auto  cfg = load("crlf.toml", error);

    EXPECT_TRUE(cfg.has_value());
    if (!cfg) return;

    // A stray '\r' surviving into a value is the interesting failure here: the
    // config would look right when printed and fail at inet_pton.
    EXPECT_EQ(cfg->exchange.host, std::string("10.0.0.1"));
    EXPECT_EQ(cfg->market_data.local_iface, std::string("10.0.0.3"));
}

// --- the secret ------------------------------------------------------------

TEST(unset_password_is_rejected) {
    set_password(nullptr);
    std::string error;
    const auto  cfg = load("good.toml", error);

    EXPECT_FALSE(cfg.has_value());
    EXPECT_TRUE(mentions(error, "NDFEX_PASSWORD"));
}

TEST(empty_password_is_rejected) {
    set_password("");
    std::string error;
    const auto  cfg = load("good.toml", error);

    EXPECT_FALSE(cfg.has_value());
    EXPECT_TRUE(mentions(error, "NDFEX_PASSWORD"));
}

TEST(password_is_never_read_from_the_file) {
    // good.toml has no password key at all; if one were added, the unknown-key
    // check must reject it rather than quietly honouring it.
    set_password("from_env");
    std::string error;
    const auto  cfg = load("good.toml", error);

    EXPECT_TRUE(cfg.has_value());
    if (!cfg) return;
    EXPECT_EQ(cfg->auth.password, std::string("from_env"));
}

// --- malformed and missing input ------------------------------------------

TEST(nonexistent_file_is_rejected) {
    expect_rejected("does_not_exist.toml");
}

TEST(empty_file_is_rejected) {
    expect_rejected("empty.toml");
}

TEST(comments_only_file_is_rejected) {
    expect_rejected("comments_only.toml");
}

TEST(missing_key_is_rejected) {
    expect_rejected("missing_key.toml");
}

TEST(missing_section_is_rejected) {
    expect_rejected("missing_section.toml");
}

TEST(unknown_key_is_rejected) {
    // The point of this one: a typo'd `prot = 1234` must be an error, not a
    // silently ignored line that leaves exchange.port at 0.
    set_password("secret");
    std::string error;
    const auto  cfg = load("unknown_key.toml", error);

    EXPECT_FALSE(cfg.has_value());
    EXPECT_TRUE(mentions(error, "prot"));
}

TEST(quoted_number_is_rejected) {
    expect_rejected("quoted_port.toml");
}

TEST(trailing_garbage_after_number_is_rejected) {
    // "1234abc" must not parse as 1234.
    expect_rejected("trailing_garbage.toml");
}

TEST(port_above_uint16_range_is_rejected) {
    expect_rejected("port_out_of_range.toml");
}

TEST(duplicate_key_is_rejected) {
    set_password("secret");
    std::string error;
    const auto  cfg = load("duplicate_key.toml", error);

    EXPECT_FALSE(cfg.has_value());
    EXPECT_TRUE(mentions(error, "duplicate"));
}

TEST(unterminated_string_is_rejected) {
    expect_rejected("unterminated_string.toml");
}

TEST(empty_value_is_rejected) {
    expect_rejected("empty_value.toml");
}

TEST(line_without_equals_is_rejected) {
    expect_rejected("no_equals.toml");
}

TEST(key_before_any_section_is_rejected) {
    expect_rejected("key_before_section.toml");
}

// --- semantic validation, not just syntax ---------------------------------

TEST(positive_pnl_floor_is_rejected) {
    // A loss limit of +5000 parses fine and would disable the kill switch.
    expect_rejected("positive_pnl_floor.toml");
}

TEST(mm_limit_above_position_limit_is_rejected) {
    expect_rejected("mm_limit_exceeds_limit.toml");
}

// --- error message quality ------------------------------------------------

TEST(line_specific_errors_name_the_line) {
    set_password("secret");
    for (const char* name : {"duplicate_key.toml", "trailing_garbage.toml",
                             "unterminated_string.toml", "no_equals.toml"}) {
        std::string error;
        const auto  cfg = load_config(fixture(name), error);
        EXPECT_FALSE(cfg.has_value());
        if (!mentions(error, "line ")) {
            ::testing::record_failure(__FILE__, __LINE__,
                                      std::string("error for '") + name
                                          + "' has no line number: " + error);
        }
    }
}
