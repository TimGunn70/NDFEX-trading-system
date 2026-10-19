#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Runtime configuration, loaded from config/bot.toml plus the environment.
//
// The password is deliberately NOT a member of any file-backed section: it is
// read from the NDFEX_PASSWORD environment variable. AUDIT.md S3 was a
// plaintext password committed in main.cpp; the fix is not "move it to a file
// we remember not to commit", it is "the value has nowhere to live in the repo
// at all".
struct Config {
    struct Exchange {
        std::string host;
        uint16_t    port{0};
        uint32_t    client_id{0};
    } exchange;

    struct Auth {
        std::string team_name;
        std::string password;   // from NDFEX_PASSWORD, never from the file
    } auth;

    struct EtfService {
        std::string url;
    } etf_service;

    struct MarketData {
        std::string live_addr;
        std::string replay_addr;
        std::string local_iface;
        uint16_t    port{0};
    } market_data;

    struct Risk {
        int32_t position_limit{0};
        int32_t mm_position_limit{0};
        double  pnl_floor{0.0};
    } risk;
};

// Parses `path` and returns a fully populated Config, or std::nullopt with a
// human-readable reason in `error` (including a line number where relevant).
//
// This is a deliberately small TOML *subset*, not a TOML implementation:
// [section] headers, key = value for quoted strings / integers / floats, and
// '#' comments. Anything outside that grammar is an error rather than a guess.
// Unknown sections and unknown keys are rejected too — a silently ignored
// `prot = 1234` would leave the port at 0 and fail much further from the cause.
//
// Reads NDFEX_PASSWORD from the environment; an unset or empty value is a
// failure, not a warning.
std::optional<Config> load_config(const std::string& path, std::string& error);
