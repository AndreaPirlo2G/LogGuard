#pragma once
// MCAP writer. Log messages are buffered, then flushed inside one Chunk
// record (optionally zstd-compressed via the runtime-loaded codec), followed
// by DataEnd/Footer. No summary/index section, so records are written in one
// pass — sufficient for incident packages and valid for Foxglove/mcap CLI.
#include <cstdint>
#include <string>
#include <vector>

namespace lg {

class McapWriter {
public:
    bool open(const std::string& path);
    // compression: "zstd" (if libzstd loads) or "none".
    void begin(const std::string& compression = "none");
    void write_log(uint32_t seq, uint16_t level, uint64_t ticket, uint64_t tid,
                   int64_t wall_ns, int64_t mono_ns,
                   const char* category, const char* req_id, const char* msg,
                   const char* fields_json = nullptr);
    // Write one self-describing manifest record (arbitrary JSON object) on the
    // dedicated /logguard/manifest channel. Safe to call zero or one time,
    // before finish(); conventionally emitted right after begin().
    void write_manifest(int64_t wall_ns, const std::string& json);
    void finish();
    ~McapWriter();
private:
    void write_record(uint8_t op, const std::vector<uint8_t>& body);
    void append_message(std::vector<uint8_t>& out, uint32_t seq, uint16_t level,
                        uint64_t ticket, uint64_t tid, int64_t wall_ns, int64_t mono_ns,
                        const char* category, const char* req_id,
                        const char* msg, const char* fields_json);
    void* f_ = nullptr; // FILE*
    std::string compression_ = "none";
    std::vector<uint8_t> chunk_; // buffered message records
};

} // namespace lg
