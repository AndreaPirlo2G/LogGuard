#pragma once
// Compare two error profiles (baseline vs candidate build) and report the
// signatures that appeared, disappeared or changed frequency. This powers
// `lgtool diff` and, more importantly, CI gating: a candidate that introduces a
// new error path or makes an existing one more frequent is rejected.
#include "diagnosis/fingerprint.hpp"

#include <string>
#include <vector>

namespace lg::diag {

enum class DiffKind { Added, Removed, FrequencyChanged };

struct DiffEntry {
    DiffKind kind;
    Signature signature;
    int baseline = 0;
    int candidate = 0;
};

struct DiffReport {
    std::vector<DiffEntry> entries;
    int added = 0;
    int removed = 0;
    int changed = 0;

    bool has_regression() const { return added > 0 || changed > 0; }
};

// Pure comparison. Ordering is deterministic (signatures are ordered).
DiffReport compare(const SignatureSet& baseline, const SignatureSet& candidate);

// Human-readable rendering (+ added, - removed, ~ changed).
std::string render(const DiffReport& report);

} // namespace lg::diag
