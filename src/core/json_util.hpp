#pragma once
// Minimal, dependency-free JSON helpers shared by the slot renderer, CLI and
// exporters. Intentionally not a general JSON DOM: logguard only needs safe
// string escaping and a few typed value emitters.
#include <cstdint>
#include <string>
#include <string_view>

namespace lg::core::json {

// Append a JSON-escaped, quoted string ("...") to `out`.
void write_escaped(std::string& out, std::string_view s);

// Append a JSON key/value pair given a raw (already JSON-encoded) value token,
// e.g. write_kv_raw(o, "n", "42") -> ,"n":42 .
void write_kv_raw(std::string& out, std::string_view key,
                  std::string_view raw_value, bool& first);

// Typed convenience emitters.
void write_kv_string(std::string& out, std::string_view key,
                     std::string_view value, bool& first);
void write_kv_int(std::string& out, std::string_view key, int64_t value,
                  bool& first);
void write_kv_bool(std::string& out, std::string_view key, bool value,
                   bool& first);

} // namespace lg::core::json
