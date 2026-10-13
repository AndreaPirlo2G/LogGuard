#include "core/json_util.hpp"

namespace lg::core::json {

void write_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

static void prefix(std::string& out, std::string_view key, bool& first) {
    if (!first) out.push_back(',');
    first = false;
    out.push_back('"');
    out += key;
    out += "\":";
}

void write_kv_raw(std::string& out, std::string_view key,
                  std::string_view raw_value, bool& first) {
    prefix(out, key, first);
    out += raw_value;
}

void write_kv_string(std::string& out, std::string_view key,
                     std::string_view value, bool& first) {
    prefix(out, key, first);
    write_escaped(out, value);
}

void write_kv_int(std::string& out, std::string_view key, int64_t value,
                  bool& first) {
    prefix(out, key, first);
    out += std::to_string(value);
}

void write_kv_bool(std::string& out, std::string_view key, bool value,
                   bool& first) {
    prefix(out, key, first);
    out += value ? "true" : "false";
}

} // namespace lg::core::json
