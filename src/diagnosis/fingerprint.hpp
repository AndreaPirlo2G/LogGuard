#pragma once
// Error fingerprints: turn a variable log message into a stable signature so
// the same bug reported with different ids/addresses/quoted values collapses to
// one entry.
//
//   "charge failed for order 9003"  -> "charge failed for order <n>"
//   "segfault at 0x7f9a1b3c ..."    -> "segfault at <hex> ..."
//
// The normalizer is intentionally conservative: it only rewrites obviously
// variable tokens, never structural words, so two genuinely different errors
// rarely alias. It is pure and allocation-bounded by the input size.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace lg::diag {

// Levels considered when building an error profile.
enum class SignatureLevel : uint8_t { Warn = 0, Error = 1, Critical = 2 };

// Normalize one message into a stable signature string.
std::string normalize_message(const std::string& msg);

// One distinct error signature and how often it occurred.
struct Signature {
    SignatureLevel level = SignatureLevel::Error;
    std::string category;    // as emitted (never empty in a stored signature)
    std::string normalized;  // normalized message

    std::string key() const; // "LEVEL\tcategory\tnormalized"
    bool operator<(const Signature& o) const { return key() < o.key(); }
    bool operator==(const Signature& o) const { return key() == o.key(); }
};

// Map of signature -> occurrence count (ordered for deterministic output).
using SignatureSet = std::map<Signature, int>;

// Parse a numeric/string level token ("ERROR"/"WARN"/"CRITICAL" or a number)
// into a SignatureLevel; returns false for levels that are not error-class.
bool level_from_text(const std::string& text, SignatureLevel& out);

const char* level_name(SignatureLevel l);

// Add one observation to a signature set. Internal categories (beginning with
// '_', e.g. _stack/_event) and non-error levels are ignored by the caller; this
// function performs the category filtering consistently. Returns true if added.
bool add_observation(SignatureSet& set, const std::string& level_text,
                     const std::string& category, const std::string& msg);

// Serialize/load a signature set as tab-separated lines (the .fp baseline
// format): "LEVEL\tcategory\tnormalized\tcount".
std::string serialize(const SignatureSet& set);
SignatureSet deserialize(const std::string& text);

} // namespace lg::diag
