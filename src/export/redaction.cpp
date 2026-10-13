#include "export/redaction.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace lg::export_ {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_atom(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
           c == '.' || c == '%' || c == '+';
}

// Masks an accepted span by overwriting with a fixed tag; we replace the span
// in an output buffer rather than mutating indices, using a build loop.
const char* tag_for(SensitiveKind k) {
    switch (k) {
        case SensitiveKind::Phone:    return "<phone>";
        case SensitiveKind::Email:    return "<email>";
        case SensitiveKind::IdCard:   return "<idcard>";
        case SensitiveKind::BankCard: return "<bankcard>";
        case SensitiveKind::IPv4:     return "<ip>";
        case SensitiveKind::Secret:   return "<secret>";
        case SensitiveKind::Custom:   return "<redacted>";
    }
    return "<redacted>";
}

} // namespace

const char* kind_tag(SensitiveKind k) { return tag_for(k); }

Redactor::Redactor() = default;

void Redactor::enable(SensitiveKind kind, bool on) {
    enabled_[static_cast<size_t>(kind)] = on;
}

void Redactor::add_custom_token(std::string token) {
    if (token.size() >= 4) custom_tokens_.push_back(std::move(token));
}

// Scanner: walks the string once; at each position tries the recognizers, and
// on a match appends the tag instead of the consumed characters. Recognizers
// return the consumed length or 0. All are conservative to limit false
// positives in ordinary log prose.
namespace {

size_t match_email(const std::string& s, size_t i) {
    size_t local = 0, j = i;
    while (j < s.size() && is_atom(s[j])) { ++j; ++local; }
    if (j >= s.size() || s[j] != '@' || local == 0) return 0;
    ++j; // @
    size_t dom = 0;
    bool had_dot = false;
    while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) ||
                            s[j] == '-' || s[j] == '.')) {
        if (s[j] == '.') had_dot = true;
        ++j; ++dom;
    }
    if (dom < 3 || !had_dot) return 0;
    return j - i;
}

size_t match_ipv4(const std::string& s, size_t i) {
    size_t j = i;
    int parts = 0;
    for (int p = 0; p < 4; ++p) {
        size_t st = j;
        int v = 0, digits = 0;
        while (j < s.size() && is_digit(s[j])) {
            v = v * 10 + (s[j] - '0');
            if (v > 255) return 0;
            ++j; ++digits;
        }
        if (digits == 0) return 0;
        (void)st;
        ++parts;
        if (p < 3) {
            if (j >= s.size() || s[j] != '.') return 0;
            ++j;
        }
    }
    // Boundary: don't eat a longer digit run (e.g. a version-ish 1.2.3.4.5).
    if (j < s.size() && is_digit(s[j])) return 0;
    if (parts == 4) return j - i;
    return 0;
}

// Consume a digit run, optionally separated by spaces/dashes in known schemes.
size_t digit_run(const std::string& s, size_t i, size_t maxlen) {
    size_t j = i;
    while (j < s.size() && is_digit(s[j]) && (j - i) < maxlen) ++j;
    return j - i;
}

// Secret assignment: keys like token/secret/apikey/password/authorization
// followed by '=' or ':' and a quoted or bare value.
size_t match_secret(const std::string& s, size_t i) {
    static const std::array<const char*, 8> keys = {
        "password", "passwd", "secret", "api_key", "apikey", "access_key",
        "authorization", "token"};
    for (const char* key : keys) {
        size_t kl = std::strlen(key);
        if (s.compare(i, kl, key) != 0) continue;
        size_t j = i + kl;
        // allow optional spaces and one separator '=' or ':'
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j >= s.size() || (s[j] != '=' && s[j] != ':')) continue;
        ++j;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '"' ||
                                s[j] == '\''))
            ++j;
        size_t vstart = j;
        // bare value until whitespace, quote, '&' or end
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '"' &&
               s[j] != '\'' && s[j] != '\n' && s[j] != '&' && s[j] != ',')
            ++j;
        if (j - vstart >= 3) return j - i;
    }
    return 0;
}

} // namespace

RedactionStats Redactor::apply(std::string& input) const {
    RedactionStats st;
    std::string out;
    out.reserve(input.size());

    auto count = [&](SensitiveKind k) {
        switch (k) {
            case SensitiveKind::Phone: ++st.phone; break;
            case SensitiveKind::Email: ++st.email; break;
            case SensitiveKind::IdCard: ++st.id_card; break;
            case SensitiveKind::BankCard: ++st.bank_card; break;
            case SensitiveKind::IPv4: ++st.ipv4; break;
            case SensitiveKind::Secret: ++st.secret; break;
            case SensitiveKind::Custom: ++st.custom; break;
        }
    };

    const std::string& s = input;
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 0;
        SensitiveKind kind{};

        // Custom exact tokens take priority.
        for (const std::string& tok : custom_tokens_) {
            if (s.compare(i, tok.size(), tok) == 0) {
                len = tok.size(); kind = SensitiveKind::Custom; break;
            }
        }

        if (len == 0 && enabled_[static_cast<size_t>(SensitiveKind::Secret)] &&
            (len = match_secret(s, i)))
            kind = SensitiveKind::Secret;

        if (len == 0 && enabled_[static_cast<size_t>(SensitiveKind::Email)] &&
            (len = match_email(s, i)))
            kind = SensitiveKind::Email;

        if (len == 0 && enabled_[static_cast<size_t>(SensitiveKind::IPv4)] &&
            (len = match_ipv4(s, i)))
            kind = SensitiveKind::IPv4;

        // Digit-based families: grab the run, then classify by length and
        // boundary so phone/id/bank don't over-match ordinary counters.
        if (len == 0 && is_digit(s[i])) {
            size_t run = digit_run(s, i, 19);
            size_t after = i + run;
            char next = after < s.size() ? s[after] : '\0';
            bool left_ok = !(i && is_digit(s[i - 1]));
            bool right_ok = !(after < s.size() && is_digit(next));

            // CN national id: 17 digits + one check char (digit or X/x).
            bool id_x = run == 17 && after < s.size() &&
                        (s[after] == 'X' || s[after] == 'x');
            size_t id_len = id_x ? 18 : run;
            size_t id_after = i + id_len;
            char id_next = id_after < s.size() ? s[id_after] : '\0';
            bool id_right_ok = !(id_after < s.size() && is_digit(id_next));

            if (run == 11 && s[i] == '1' && s[i + 1] >= '3' && s[i + 1] <= '9' &&
                enabled_[static_cast<size_t>(SensitiveKind::Phone)] && left_ok &&
                right_ok) {
                len = run; kind = SensitiveKind::Phone;
            } else if ((run == 18 || id_x) &&
                       enabled_[static_cast<size_t>(SensitiveKind::IdCard)] &&
                       left_ok && id_right_ok) {
                len = id_len; kind = SensitiveKind::IdCard;
            } else if (run >= 13 && run <= 19 &&
                       enabled_[static_cast<size_t>(SensitiveKind::BankCard)] &&
                       left_ok && right_ok) {
                len = run; kind = SensitiveKind::BankCard;
            }
        }

        if (len) {
            out += tag_for(kind);
            count(kind);
            i += len;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    input.swap(out);
    return st;
}

std::string Redactor::redact_copy(std::string_view input) const {
    std::string copy(input);
    apply(copy);
    return copy;
}

} // namespace lg::export_
