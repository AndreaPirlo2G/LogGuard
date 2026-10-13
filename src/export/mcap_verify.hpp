// Defensive, bounds-checked MCAP container validator/parser.
//
// This is the hardened counterpart to the hand-written writer: every read is
// bounds checked, declared lengths are capped, and a corrupt or truncated
// record stream is reported instead of causing out-of-bounds access. It is
// safe to feed attacker-controlled bytes (it is the natural fuzz target).
//
// Only the subset LogGuard emits is semantically interpreted (Header, Schema,
// Channel, Chunk (optionally zstd), Message, DataEnd, Footer), but the walk is
// generic enough to skip unknown opcodes correctly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lg {
namespace export_ {

struct VerifyIssue {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    std::string message;
    size_t offset = 0; // byte offset in the scanned buffer, 0 if unknown
};

struct VerifyReport {
    bool ok = false;                 // magic present and scan reached Footer/DataEnd
    bool has_footer = false;
    bool has_data_end = false;
    bool has_header = false;
    uint64_t header_profile = 0;     // profile string length actually parsed
    uint32_t schema_count = 0;
    uint32_t channel_count = 0;
    uint32_t message_count = 0;   // /logguard/logs messages only
    uint32_t manifest_count = 0;  // /logguard/manifest messages
    uint32_t chunk_count = 0;
    uint32_t bad_chunks = 0;         // truncated/undecompressable chunks
    uint64_t parsed_bytes = 0;       // how much of the input was walked
    std::vector<VerifyIssue> issues;

    void error(const std::string& m, size_t off = 0) {
        issues.push_back({VerifyIssue::Severity::Error, m, off});
    }
    void warn(const std::string& m, size_t off = 0) {
        issues.push_back({VerifyIssue::Severity::Warning, m, off});
    }
    bool has_error() const;
    std::string render() const;
};

class McapVerifier {
public:
    // Hard caps keep a hostile file from forcing unbounded allocation.
    static constexpr uint64_t kMaxRecordLen = 512ull * 1024 * 1024; // 512 MiB
    static constexpr uint64_t kMaxChunkUncompressed =
        512ull * 1024 * 1024; // 512 MiB
    static constexpr uint32_t kMaxNesting = 8;

    // Verify an in-memory MCAP image. When |allow_trailing| is false, bytes
    // after the Footer record are reported as a warning.
    static VerifyReport verify(const uint8_t* data, size_t size,
                               bool allow_trailing = false);

    // Convenience: read a file from disk then verify. Returns false (with an
    // error issue) if the file cannot be opened.
    static VerifyReport verify_file(const std::string& path,
                                    bool allow_trailing = false);

    // Decode every /logguard/logs message payload into UTF-8 strings, walking
    // top-level records and decompressing chunks. Used by tools; empty on
    // hard failure. |out_json| receives raw JSON payloads in file order.
    struct LogMessage {
        uint64_t log_time_ns = 0;
        uint16_t channel_id = 0;
        std::string json;
    };
    static bool extract_logs(const uint8_t* data, size_t size,
                             std::vector<LogMessage>& out,
                             VerifyReport* report = nullptr);
    static bool extract_logs_file(const std::string& path,
                                  std::vector<LogMessage>& out,
                                  VerifyReport* report = nullptr);

    // Extract the raw JSON of the provenance manifest (/logguard/manifest).
    // Returns false if the package carries no manifest. The last manifest is
    // returned (a package normally has exactly one).
    static bool extract_manifest_file(const std::string& path,
                                      std::string& json_out);
};

} // namespace export_
} // namespace lg
