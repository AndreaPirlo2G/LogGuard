#include "diagnosis/signature_diff.hpp"

#include <cstdio>
#include <set>

namespace lg::diag {

DiffReport compare(const SignatureSet& baseline, const SignatureSet& candidate) {
    DiffReport rep;
    std::set<Signature> keys;
    for (const auto& [s, n] : baseline) { (void)n; keys.insert(s); }
    for (const auto& [s, n] : candidate) { (void)n; keys.insert(s); }

    auto count_of = [](const SignatureSet& set, const Signature& s) {
        auto it = set.find(s);
        return it == set.end() ? 0 : it->second;
    };

    for (const Signature& k : keys) {
        int ca = count_of(baseline, k);
        int cb = count_of(candidate, k);
        if (ca == 0 && cb > 0) {
            rep.entries.push_back({DiffKind::Added, k, 0, cb});
            ++rep.added;
        } else if (cb == 0 && ca > 0) {
            rep.entries.push_back({DiffKind::Removed, k, ca, 0});
            ++rep.removed;
        } else if (ca != cb) {
            rep.entries.push_back({DiffKind::FrequencyChanged, k, ca, cb});
            ++rep.changed;
        }
    }
    return rep;
}

std::string render(const DiffReport& report) {
    std::string out;
    char buf[64];
    for (const DiffEntry& e : report.entries) {
        const std::string k = e.signature.key();
        switch (e.kind) {
            case DiffKind::Added:
                out += "+ " + k + "  (";
                std::snprintf(buf, sizeof(buf), "%d", e.candidate);
                out += buf; out += ")\n";
                break;
            case DiffKind::Removed:
                out += "- " + k + "  (was ";
                std::snprintf(buf, sizeof(buf), "%d", e.baseline);
                out += buf; out += ")\n";
                break;
            case DiffKind::FrequencyChanged:
                out += "~ " + k + "  ";
                std::snprintf(buf, sizeof(buf), "%d -> %d", e.baseline, e.candidate);
                out += buf; out += "\n";
                break;
        }
    }
    std::snprintf(buf, sizeof(buf), "added=%d removed=%d changed=%d\n",
                  report.added, report.removed, report.changed);
    out += buf;
    return out;
}

} // namespace lg::diag
