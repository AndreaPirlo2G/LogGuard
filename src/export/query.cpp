#include "export/query.hpp"

#include "export/slot_render.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace lg {
namespace export_ {

namespace {

std::string slot_category(const lg::core::Slot& s) {
    return std::string(s.category, s.cat_len);
}
std::string slot_request(const lg::core::Slot& s) {
    return std::string(s.req_id, s.req_len);
}

// Plain text against which the `contains` clause is tested: the rendered
// message/stack/kv line, so text search behaves like what the user actually
// sees in an exported package.
std::string searchable_text(const lg::core::Slot& s) {
    RenderedSlot r = render_slot(s);
    std::string t = r.message;
    if (!r.fields_json.empty()) {
        t.push_back(' ');
        t += r.fields_json;
    }
    return t;
}

bool all_tokens_present(const std::string& haystack,
                        const std::string& tokens) {
    size_t i = 0;
    while (i < tokens.size()) {
        while (i < tokens.size() &&
               std::isspace(static_cast<unsigned char>(tokens[i])))
            ++i;
        size_t start = i;
        while (i < tokens.size() &&
               !std::isspace(static_cast<unsigned char>(tokens[i])))
            ++i;
        if (i > start) {
            // find(substring-data, position, length): match exactly this
            // token (tokens.data()+start is not NUL-terminated at the token).
            if (haystack.find(tokens.data() + start, 0, i - start) ==
                std::string::npos)
                return false;
        }
    }
    return true;
}

int level_from_name(const std::string& name) {
    static const char* names[] = {"TRACE", "DEBUG", "INFO",
                                  "WARN",  "ERROR", "CRITICAL"};
    for (int i = 0; i < 6; ++i)
        if (name == names[i]) return i;
    return -1;
}

} // namespace

bool matches(const lg::core::Slot& s, const QuerySpec& q) {
    if (q.from_ns != 0 && s.mono_ns < q.from_ns) return false;
    if (q.to_ns != 0 && s.mono_ns > q.to_ns) return false;
    if (q.min_level >= 0 && static_cast<int>(s.level) < q.min_level)
        return false;
    if (!q.category.empty() && slot_category(s) != q.category) return false;
    if (!q.request_id.empty() && slot_request(s) != q.request_id)
        return false;
    if (q.tid != 0 && s.tid != q.tid) return false;
    if (!q.contains.empty()) {
        const std::string text = searchable_text(s);
        if (q.match_all_tokens) {
            if (!all_tokens_present(text, q.contains)) return false;
        } else if (text.find(q.contains) == std::string::npos) {
            return false;
        }
    }
    return true;
}

std::vector<QueryHit> query(const std::vector<lg::core::Slot>& slots,
                            const QuerySpec& q) {
    std::vector<QueryHit> hits;
    if (!q.enabled()) {
        hits.reserve(slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
            hits.push_back(QueryHit{i, slots[i]});
        return hits;
    }
    for (size_t i = 0; i < slots.size(); ++i)
        if (matches(slots[i], q)) hits.push_back(QueryHit{i, slots[i]});
    return hits;
}

bool parse_query(const std::string& expr, QuerySpec& out,
                 std::string* error) {
    out = QuerySpec{};
    size_t i = 0;
    while (i < expr.size()) {
        while (i < expr.size() &&
               std::isspace(static_cast<unsigned char>(expr[i])))
            ++i;
        if (i >= expr.size()) break;
        size_t start = i;
        while (i < expr.size() &&
               !std::isspace(static_cast<unsigned char>(expr[i])))
            ++i;
        std::string tok = expr.substr(start, i - start);

        const size_t eq = tok.find('=');
        if (eq == std::string::npos) {
            // Bare word: append to text search (multi-token AND).
            if (!out.contains.empty()) out.contains.push_back(' ');
            out.contains += tok;
            out.match_all_tokens = true;
            continue;
        }
        std::string key = tok.substr(0, eq);
        std::string val = tok.substr(eq + 1);
        // Tolerate "key>=value" (only minimum filters are supported).
        while (!key.empty() &&
               (key.back() == '>' || key.back() == '=' || key.back() == '<'))
            key.pop_back();
        if (key == "level" || key == "lvl") {
            int lv = level_from_name(val);
            if (lv < 0) {
                if (error)
                    *error = "unknown level '" + val +
                             "' (TRACE..CRITICAL)";
                return false;
            }
            out.min_level = lv;
        } else if (key == "category" || key == "cat") {
            out.category = val;
        } else if (key == "req" || key == "request") {
            out.request_id = val;
        } else if (key == "tid") {
            out.tid = static_cast<uint64_t>(
                std::strtoull(val.c_str(), nullptr, 10));
        } else if (key == "from") {
            out.from_ns = std::strtoull(val.c_str(), nullptr, 10);
        } else if (key == "to") {
            out.to_ns = std::strtoull(val.c_str(), nullptr, 10);
        } else if (key == "text" || key == "q") {
            if (!out.contains.empty()) {
                out.contains.push_back(' ');
                out.match_all_tokens = true;
            }
            out.contains += val;
        }
        // Unknown keys are ignored so newer CLIs work against older libs.
    }
    return true;
}

std::string render_hit(const QueryHit& h) {
    RenderedSlot r = render_slot(h.slot);
    static const char* names[] = {"TRACE", "DEBUG", "INFO",
                                  "WARN",  "ERROR", "CRITICAL"};
    const char* lv =
        r.level <= 5 ? names[r.level] : "?";
    std::ostringstream os;
    os << "[" << lv << "]";
    if (!r.category.empty()) os << " " << r.category;
    if (!r.request_id.empty()) os << " (" << r.request_id << ")";
    os << " tid=" << h.slot.tid;
    os << " " << r.message;
    if (!r.fields_json.empty()) os << " " << r.fields_json;
    return os.str();
}

} // namespace export_
} // namespace lg
