#include "diagnosis/fingerprint.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace lg::diag {

std::string normalize_message(const std::string& msg) {
    std::string out;
    out.reserve(msg.size());
    for (size_t i = 0; i < msg.size();) {
        unsigned char c = static_cast<unsigned char>(msg[i]);
        // Hexadecimal literal: 0x....
        if (c == '0' && i + 1 < msg.size() &&
            (msg[i + 1] == 'x' || msg[i + 1] == 'X')) {
            i += 2;
            while (i < msg.size() &&
                   std::strchr("0123456789abcdefABCDEF", msg[i]))
                ++i;
            out += "<hex>";
            continue;
        }
        // Decimal number (keep a possible decimal point/digit run together).
        if (std::isdigit(c)) {
            while (i < msg.size() && std::strchr("0123456789.", msg[i])) ++i;
            out += "<n>";
            continue;
        }
        // Quoted literal value.
        if (c == '"' || c == '\'') {
            char q = msg[i++];
            while (i < msg.size() && msg[i] != q) ++i;
            if (i < msg.size()) ++i;
            out += "<str>";
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
    // Collapse runs of the same numeric placeholder ("<n>.<n>" -> "<n>").
    std::string collapsed;
    collapsed.reserve(out.size());
    for (size_t i = 0; i < out.size();) {
        if (i + 3 <= out.size() && out.compare(i, 3, "<n>") == 0) {
            size_t j = i;
            while (j + 3 <= out.size() && out.compare(j, 3, "<n>") == 0)
                j += 3;
            collapsed += "<n>";
            i = j;
        } else {
            collapsed.push_back(out[i]);
            ++i;
        }
    }
    return collapsed;
}

std::string Signature::key() const {
    return std::string(level_name(level)) + "\t" + category + "\t" + normalized;
}

bool level_from_text(const std::string& text, SignatureLevel& out) {
    if (text == "WARN" || text == "WARNING") { out = SignatureLevel::Warn; return true; }
    if (text == "ERROR") { out = SignatureLevel::Error; return true; }
    if (text == "CRITICAL" || text == "FATAL") { out = SignatureLevel::Critical; return true; }
    // Numeric severity (our levels: 3=WARN 4=ERROR 5=CRITICAL).
    if (!text.empty() && std::all_of(text.begin(), text.end(),
                                     [](char c){ return std::isdigit(static_cast<unsigned char>(c)); })) {
        int n = std::atoi(text.c_str());
        if (n == 3) { out = SignatureLevel::Warn; return true; }
        if (n == 4) { out = SignatureLevel::Error; return true; }
        if (n >= 5) { out = SignatureLevel::Critical; return true; }
    }
    return false;
}

const char* level_name(SignatureLevel l) {
    switch (l) {
        case SignatureLevel::Warn: return "WARN";
        case SignatureLevel::Error: return "ERROR";
        case SignatureLevel::Critical: return "CRITICAL";
    }
    return "ERROR";
}

bool add_observation(SignatureSet& set, const std::string& level_text,
                     const std::string& category, const std::string& msg) {
    // Internal markers/stacks are not user errors.
    if (!category.empty() && category[0] == '_') return false;
    SignatureLevel lvl;
    if (!level_from_text(level_text, lvl)) return false;
    Signature s;
    s.level = lvl;
    s.category = category;
    s.normalized = normalize_message(msg);
    ++set[s];
    return true;
}

std::string serialize(const SignatureSet& set) {
    std::string out;
    for (const auto& [sig, n] : set) {
        out += sig.key();
        out.push_back('\t');
        out += std::to_string(n);
        out.push_back('\n');
    }
    return out;
}

SignatureSet deserialize(const std::string& text) {
    SignatureSet set;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        size_t tab3 = line.rfind('\t');
        if (tab3 == std::string::npos) continue;
        int n = std::atoi(line.c_str() + tab3 + 1);
        std::string head = line.substr(0, tab3);
        size_t tab2 = head.rfind('\t');
        if (tab2 == std::string::npos) continue;
        std::string cat_sig = head.substr(tab2 + 1);
        std::string level_cat = head.substr(0, tab2);
        size_t tab1 = level_cat.find('\t');
        if (tab1 == std::string::npos) continue;
        SignatureLevel lvl;
        if (!level_from_text(level_cat.substr(0, tab1), lvl)) continue;
        Signature s;
        s.level = lvl;
        // category may itself contain no tab; split key() produced
        // "LEVEL\tcategory\tnormalized", and normalized can contain spaces but
        // not tabs. Recover category as the middle field.
        std::string middle = level_cat.substr(tab1 + 1);
        // middle is "category"; cat_sig is the normalized message here because
        // key() is level\tcategory\tnormalized and we split from the right on
        // the final tab (count), then second-rightmost tab.
        s.category = middle;
        s.normalized = cat_sig;
        set[s] += n;
    }
    return set;
}

} // namespace lg::diag
