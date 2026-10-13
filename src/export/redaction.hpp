#pragma once
// Privacy redaction for incident packages.
//
// Products that ship black-box files to support must not leak PII or secrets.
// The redactor rewrites recognized sensitive tokens in rendered text BEFORE it
// is serialized to an .lgp, so the on-disk package is safe to hand over. It is
// deliberately regex-free (a small deterministic scanner) to stay predictable,
// allocation-bounded and auditable — redaction bugs are security bugs.
//
// Recognized families:
//   phone        11-digit CN mobile numbers (1[3-9]xxxxxxxxx)
//   email        local@domain
//   id_card      17 digits + digit/X (CN national id)
//   bank_card    13-19 digit run (checked against digit density context)
//   ipv4         dotted-quad
//   secret_key   assignments like token=..., api_key:"...", authorization
//
// Custom additional literal tokens can be supplied at runtime.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lg::export_ {

enum class SensitiveKind : uint8_t {
    Phone,
    Email,
    IdCard,
    BankCard,
    IPv4,
    Secret,
    Custom,
};

struct RedactionStats {
    int phone = 0;
    int email = 0;
    int id_card = 0;
    int bank_card = 0;
    int ipv4 = 0;
    int secret = 0;
    int custom = 0;
    int total() const {
        return phone + email + id_card + bank_card + ipv4 + secret + custom;
    }
};

class Redactor {
public:
    Redactor();

    // Enable/disable individual families (all enabled by default).
    void enable(SensitiveKind kind, bool on);

    // Add a literal secret token (e.g. an API key value) that must be masked
    // wherever it appears. Tokens shorter than 4 chars are ignored to avoid
    // over-masking.
    void add_custom_token(std::string token);

    // Redact `input` in place. Returns the hit counts.
    RedactionStats apply(std::string& input) const;

    // Convenience: return a redacted copy.
    std::string redact_copy(std::string_view input) const;

private:
    bool enabled_[8] = {true, true, true, true, true, true, true};
    std::vector<std::string> custom_tokens_;
};

const char* kind_tag(SensitiveKind k); // "<phone>", "<email>", ...

} // namespace lg::export_
