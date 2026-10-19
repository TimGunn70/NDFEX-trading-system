#include "config.h"

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace {

// One key = value pair as it appeared in the file, with the line it came from
// so every error message can point at a place in the file.
struct Raw {
    std::string value;
    int         line{0};
};

using RawMap = std::map<std::string, Raw>;   // "section.key" -> Raw

std::string trim(const std::string& s) {
    static const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Truncates at the first '#' that is not inside a double-quoted string, so
// `port = 1234  # exchange` works but `url = "http://x/#frag"` is left alone.
// Escape sequences are not supported inside strings; a backslash is literal.
std::string strip_comment(const std::string& line) {
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_quote = !in_quote;
        else if (line[i] == '#' && !in_quote) return line.substr(0, i);
    }
    return line;
}

std::string at_line(int line) {
    return " (line " + std::to_string(line) + ")";
}

// --- typed extraction -----------------------------------------------------
// Each take_* erases the key it consumed, so whatever remains in the map at
// the end is by definition a key the schema does not know about.

bool take_raw(RawMap& kv, const std::string& name, Raw& out, std::string& error) {
    const auto it = kv.find(name);
    if (it == kv.end()) {
        error = "missing required key '" + name + "'";
        return false;
    }
    out = it->second;
    kv.erase(it);
    return true;
}

bool take_string(RawMap& kv, const std::string& name, std::string& out, std::string& error) {
    Raw raw;
    if (!take_raw(kv, name, raw, error)) return false;

    if (raw.value.size() < 2 || raw.value.front() != '"' || raw.value.back() != '"') {
        error = "value for '" + name + "' must be a double-quoted string" + at_line(raw.line);
        return false;
    }
    out = raw.value.substr(1, raw.value.size() - 2);
    if (out.find('"') != std::string::npos) {
        error = "unexpected quote inside value for '" + name + "'" + at_line(raw.line);
        return false;
    }
    return true;
}

// Parses a signed integer with no trailing characters, then range-checks it
// against [lo, hi]. from_chars is used rather than stoi/strtol because it does
// not throw, ignores the locale, and reports exactly where it stopped — so
// "1234abc" is an error rather than 1234.
bool take_integer(RawMap& kv, const std::string& name, long long lo, long long hi,
                  long long& out, std::string& error) {
    Raw raw;
    if (!take_raw(kv, name, raw, error)) return false;

    if (!raw.value.empty() && raw.value.front() == '"') {
        error = "value for '" + name + "' must be a number, not a quoted string" + at_line(raw.line);
        return false;
    }

    long long v = 0;
    const char* begin = raw.value.data();
    const char* end   = raw.value.data() + raw.value.size();
    const auto  res   = std::from_chars(begin, end, v);
    if (res.ec != std::errc{} || res.ptr != end) {
        error = "value for '" + name + "' is not a valid integer: '" + raw.value + "'"
                + at_line(raw.line);
        return false;
    }
    if (v < lo || v > hi) {
        error = "value for '" + name + "' out of range [" + std::to_string(lo) + ", "
                + std::to_string(hi) + "]: " + raw.value + at_line(raw.line);
        return false;
    }
    out = v;
    return true;
}

bool take_double(RawMap& kv, const std::string& name, double& out, std::string& error) {
    Raw raw;
    if (!take_raw(kv, name, raw, error)) return false;

    if (!raw.value.empty() && raw.value.front() == '"') {
        error = "value for '" + name + "' must be a number, not a quoted string" + at_line(raw.line);
        return false;
    }

    // std::from_chars for floating point is not reliably available across the
    // libc++ / libstdc++ versions this has to build on, so strtod it is —
    // with the errno and end-pointer checks that make strtod safe.
    errno = 0;
    char*        endptr = nullptr;
    const double v      = std::strtod(raw.value.c_str(), &endptr);
    if (endptr != raw.value.c_str() + raw.value.size() || endptr == raw.value.c_str()) {
        error = "value for '" + name + "' is not a valid number: '" + raw.value + "'"
                + at_line(raw.line);
        return false;
    }
    if (errno == ERANGE) {
        error = "value for '" + name + "' is out of range for a double: '" + raw.value + "'"
                + at_line(raw.line);
        return false;
    }
    out = v;
    return true;
}

}  // namespace

std::optional<Config> load_config(const std::string& path, std::string& error) {
    error.clear();

    std::ifstream in(path);
    if (!in) {
        error = "cannot open config file '" + path + "'";
        return std::nullopt;
    }

    RawMap      kv;
    std::string section;
    std::string line;
    int         lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        const std::string text = trim(strip_comment(line));
        if (text.empty()) continue;

        if (text.front() == '[') {
            if (text.back() != ']') {
                error = "unterminated section header" + at_line(lineno);
                return std::nullopt;
            }
            section = trim(text.substr(1, text.size() - 2));
            if (section.empty()) {
                error = "empty section name" + at_line(lineno);
                return std::nullopt;
            }
            continue;
        }

        const size_t eq = text.find('=');
        if (eq == std::string::npos) {
            error = "expected 'key = value'" + at_line(lineno);
            return std::nullopt;
        }

        const std::string key = trim(text.substr(0, eq));
        const std::string val = trim(text.substr(eq + 1));
        if (key.empty()) {
            error = "empty key name" + at_line(lineno);
            return std::nullopt;
        }
        if (val.empty()) {
            error = "missing value for '" + key + "'" + at_line(lineno);
            return std::nullopt;
        }
        if (section.empty()) {
            error = "key '" + key + "' appears before any [section]" + at_line(lineno);
            return std::nullopt;
        }
        // A value that opens a quote but never closes it. Checked here rather
        // than in take_string so the error names the line, not just the key.
        if (val.front() == '"' && (val.size() < 2 || val.back() != '"')) {
            error = "unterminated string for '" + key + "'" + at_line(lineno);
            return std::nullopt;
        }

        const std::string full = section + "." + key;
        const auto        dup  = kv.find(full);
        if (dup != kv.end()) {
            error = "duplicate key '" + full + "'" + at_line(lineno)
                    + ", first defined" + at_line(dup->second.line);
            return std::nullopt;
        }
        kv.emplace(full, Raw{val, lineno});
    }

    Config    cfg;
    long long n = 0;

    if (!take_string (kv, "exchange.host",              cfg.exchange.host,          error)) return std::nullopt;
    if (!take_integer(kv, "exchange.port",       1, 65535,               n, error)) return std::nullopt;
    cfg.exchange.port = static_cast<uint16_t>(n);
    if (!take_integer(kv, "exchange.client_id",  0, UINT32_MAX,          n, error)) return std::nullopt;
    cfg.exchange.client_id = static_cast<uint32_t>(n);

    if (!take_string (kv, "auth.team_name",             cfg.auth.team_name,         error)) return std::nullopt;

    if (!take_string (kv, "etf_service.url",            cfg.etf_service.url,        error)) return std::nullopt;

    if (!take_string (kv, "market_data.live_addr",      cfg.market_data.live_addr,   error)) return std::nullopt;
    if (!take_string (kv, "market_data.replay_addr",    cfg.market_data.replay_addr, error)) return std::nullopt;
    if (!take_string (kv, "market_data.local_iface",    cfg.market_data.local_iface, error)) return std::nullopt;
    if (!take_integer(kv, "market_data.port",    1, 65535,               n, error)) return std::nullopt;
    cfg.market_data.port = static_cast<uint16_t>(n);

    if (!take_integer(kv, "risk.position_limit",    1, INT32_MAX,        n, error)) return std::nullopt;
    cfg.risk.position_limit = static_cast<int32_t>(n);
    if (!take_integer(kv, "risk.mm_position_limit", 1, INT32_MAX,        n, error)) return std::nullopt;
    cfg.risk.mm_position_limit = static_cast<int32_t>(n);
    if (!take_double (kv, "risk.pnl_floor",             cfg.risk.pnl_floor,          error)) return std::nullopt;

    // Anything still in the map is a key the schema does not recognise. This is
    // the check that catches a typo like `prot = 1234`, which would otherwise
    // leave the port at 0 and fail later, far from the cause.
    if (!kv.empty()) {
        const auto& first = *kv.begin();
        error = "unknown key '" + first.first + "'" + at_line(first.second.line);
        return std::nullopt;
    }

    if (cfg.risk.pnl_floor >= 0.0) {
        error = "risk.pnl_floor must be negative (it is a loss limit), got "
                + std::to_string(cfg.risk.pnl_floor);
        return std::nullopt;
    }
    if (cfg.risk.mm_position_limit > cfg.risk.position_limit) {
        error = "risk.mm_position_limit (" + std::to_string(cfg.risk.mm_position_limit)
                + ") exceeds risk.position_limit (" + std::to_string(cfg.risk.position_limit) + ")";
        return std::nullopt;
    }

    // Secret comes from the environment only. Checked after the file so that a
    // malformed config is reported on its own terms first.
    const char* pw = std::getenv("NDFEX_PASSWORD");
    if (pw == nullptr || *pw == '\0') {
        error = "NDFEX_PASSWORD is unset or empty; export it before running "
                "(see .env.example) — it is never read from the config file";
        return std::nullopt;
    }
    cfg.auth.password = pw;

    return cfg;
}
