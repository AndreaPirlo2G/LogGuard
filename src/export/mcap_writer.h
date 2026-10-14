#pragma once
// MCAP writer with a built-in chunk time index.
//
// Log messages are appended to an in-memory chunk buffer; once the uncompressed
// buffer reaches kChunkTargetBytes it is flushed to disk (optionally
// zstd-compressed via the runtime-loaded codec) and a ChunkIndex entry is
// recorded. At finish() a custom CHUNK_INDEX summary section is written
// (before DataEnd/Footer) holding each chunk's monotonic time span, file
// offset, compressed/uncompressed size and compression. Readers that know the
// section can binary-search it to answer a time-window query by inflating only
// the intersecting chunks (O(log n) chunks instead of a full linear scan);
// readers that do not recognise the opcode simply skip it, so the files stay
// valid for Foxglove / mcap CLI and for older LogGuard tooling.
#include <cstdint>
#include <string>
#include <vector>

namespace lg {

class McapWriter {
public:
    bool open(const std::string& path);
    // compression: "zstd" (if libzstd loads) or "none".
    void begin(const std::string& compression = "none");
    void begin(const std::string& compression, size_t target_chunk_bytes);
    void write_log(uint32_t seq, uint16_t level, uint64_t ticket, uint64_t tid,
                   int64_t wall_ns, int64_t mono_ns,
                   const char* category, const char* req_id, const char* msg,
                   const char* fields_json = nullptr);
    // Write one self-describing manifest record (arbitrary JSON object) on the
    // dedicated /logguard/manifest channel. Safe to call zero or one time,
    // before finish(); conventionally emitted right after begin().
    void write_manifest(int64_t wall_ns, const std::string& json);
    // Append a /logguard/logs message whose body is an already-built JSON
    // object matching schema v3. Used by offline package rewriters (e.g.
    // `lgtool redact`) that preserve the original framing fields.
    void write_log_raw(uint32_t seq, int64_t wall_ns, int64_t mono_ns,
                       const std::string& json_payload);
    void finish();
    ~McapWriter();

    // Flush the current chunk once its uncompressed payload reaches this many
    // bytes. Smaller packages stay a single chunk (identical on-disk layout to
    // a non-indexed writer except for the trailing summary section).
    static constexpr uint64_t kChunkTargetBytes = 2u * 1024 * 1024; // 2 MiB

private:
    struct ChunkMeta {
        int64_t mono_start = 0;
        int64_t mono_end = 0;
        uint64_t file_offset = 0; // absolute offset of the chunk record header
        uint64_t compressed_size = 0;
        uint64_t uncompressed_size = 0;
        uint8_t compression = 0; // 0 = none, 1 = zstd
    };

    void write_record(uint8_t op, const std::vector<uint8_t>& body);
    void flush_chunk();
    void append_message(std::vector<uint8_t>& out, uint32_t seq, uint16_t level,
                        uint64_t ticket, uint64_t tid, int64_t wall_ns, int64_t mono_ns,
                        const char* category, const char* req_id,
                        const char* msg, const char* fields_json);
    void* f_ = nullptr; // FILE*
    size_t target_bytes_ = kChunkTargetBytes; // effective rollover threshold
    std::string compression_ = "none";
    std::vector<uint8_t> chunk_; // buffered message records for current chunk
    std::vector<uint8_t> manifest_rec_; // framed manifest message (prepended to first chunk)
    std::vector<ChunkMeta> index_;
    int64_t cur_mono_start_ = 0;
    int64_t cur_mono_end_ = 0;
    bool cur_has_span_ = false;
    uint32_t seq_cursor_ = 0;
};

} // namespace lg
