#include "mcap_writer.h"
#include "zstd_codec.h"

#include <cstdio>
#include <cstring>
#include <vector>
namespace lg {

namespace {

constexpr uint8_t OP_HEADER   = 0x01;
constexpr uint8_t OP_SCHEMA   = 0x06;
constexpr uint8_t OP_CHANNEL  = 0x07;
constexpr uint8_t OP_MESSAGE  = 0x05;
constexpr uint8_t OP_CHUNK    = 0x0D;
constexpr uint8_t OP_DATA_END = 0x0F;
constexpr uint8_t OP_FOOTER   = 0x0E;
// LogGuard-specific summary section: one entry per on-disk chunk. Unknown to
// standard readers, which must skip unrecognised opcodes.
constexpr uint8_t OP_CHUNK_INDEX = 0x40; // '@'
// CHUNK_INDEX body:
//   magic[4]   = "LGIX"
//   version u8 = 1
//   count   u32
//   count * { mono_start i64, mono_end i64, file_offset u64,
//             compressed_size u64, uncompressed_size u64, compression u8 }
const uint8_t CHUNK_INDEX_MAGIC[4] = {'L', 'G', 'I', 'X'};
constexpr uint8_t CHUNK_INDEX_VERSION = 1;

const uint8_t MAGIC[9] = {0x89, 'M', 'C', 'A', 'P', '\r', '\n', 0x1A, '\n'};

void put_u16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void put_u32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void put_u64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void put_i64(std::vector<uint8_t>& b, int64_t v)  { put_u64(b, static_cast<uint64_t>(v)); }
void put_string(std::vector<uint8_t>& b, const char* s, uint32_t n) { put_u32(b, n); b.insert(b.end(), s, s + n); }
void put_string(std::vector<uint8_t>& b, const std::string& s) { put_string(b, s.data(), uint32_t(s.size())); }

void put_json_string(std::vector<uint8_t>& b, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  b.insert(b.end(), {'\\', '"'}); break;
            case '\\': b.insert(b.end(), {'\\', '\\'}); break;
            case '\n': b.insert(b.end(), {'\\', 'n'}); break;
            case '\r': b.insert(b.end(), {'\\', 'r'}); break;
            case '\t': b.insert(b.end(), {'\\', 't'}); break;
            default:
                if (c < 0x20) { char hex[8]; std::snprintf(hex, sizeof(hex), "\\u%04x", c); b.insert(b.end(), hex, hex + 6); }
                else b.push_back(c);
        }
    }
}

const char* level_name(uint16_t level) {
    static const char* names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"};
    return level <= 5 ? names[level] : "?";
}

} // namespace

bool McapWriter::open(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    return f_ != nullptr;
}

McapWriter::~McapWriter() { if (f_) std::fclose(static_cast<FILE*>(f_)); }

void McapWriter::write_record(uint8_t op, const std::vector<uint8_t>& body) {
    FILE* fp = static_cast<FILE*>(f_);
    uint8_t hdr[9]; hdr[0] = op;
    uint64_t len = body.size();
    std::memcpy(hdr + 1, &len, 8);
    std::fwrite(hdr, 1, 9, fp);
    if (!body.empty()) std::fwrite(body.data(), 1, body.size(), fp);
}

void McapWriter::begin(const std::string& compression) {
    begin(compression, 0);
}

void McapWriter::begin(const std::string& compression, size_t target_chunk_bytes) {
    compression_ = compression;
    constexpr size_t kMinTarget = 4 * 1024;
    constexpr size_t kMaxTarget = 64ull * 1024 * 1024;
    if (target_chunk_bytes == 0)
        target_bytes_ = kChunkTargetBytes;
    else if (target_chunk_bytes < kMinTarget)
        target_bytes_ = kMinTarget;
    else if (target_chunk_bytes > kMaxTarget)
        target_bytes_ = kMaxTarget;
    else
        target_bytes_ = target_chunk_bytes;
    FILE* fp = static_cast<FILE*>(f_);
    std::fwrite(MAGIC, 1, sizeof(MAGIC), fp);
    {
        std::vector<uint8_t> b;
        put_string(b, "logguard/1");
        put_string(b, "logguard/0.3");
        write_record(OP_HEADER, b);
    }
    {
        const std::string schema =
            R"({"type":"object","properties":{"ticket":{"type":"integer"},)"
            R"("ts_ns":{"type":"integer"},"mono_ns":{"type":"integer"},)"
            R"("tid":{"type":"integer"},"level":{"type":"string"},)"
            R"("category":{"type":"string"},"request_id":{"type":"string"},)"
            R"("msg":{"type":"string"},"fields":{"type":"object"}}})";
        std::vector<uint8_t> b;
        put_u16(b, 1);
        put_string(b, "logguard/log-record/v3");
        put_string(b, "jsonschema");
        put_string(b, schema);
        write_record(OP_SCHEMA, b);
    }
    {
        std::vector<uint8_t> b;
        put_u16(b, 1); put_u16(b, 1);
        put_string(b, "/logguard/logs");
        put_string(b, "json");
        put_u32(b, 0);
        write_record(OP_CHANNEL, b);
    }
    {
        // Schema 2 / channel 2: free-form package manifest (provenance).
        std::vector<uint8_t> b;
        put_u16(b, 2);
        put_string(b, "logguard/manifest/v1");
        put_string(b, "jsonschema");
        put_string(b, R"({"type":"object"})");
        write_record(OP_SCHEMA, b);
    }
    {
        std::vector<uint8_t> b;
        put_u16(b, 2); put_u16(b, 2);
        put_string(b, "/logguard/manifest");
        put_string(b, "json");
        put_u32(b, 0);
        write_record(OP_CHANNEL, b);
    }
}

void McapWriter::append_message(std::vector<uint8_t>& out, uint32_t seq, uint16_t level,
                                uint64_t ticket, uint64_t tid, int64_t wall_ns, int64_t mono_ns,
                                const char* category, const char* req_id,
                                const char* msg, const char* fields_json) {
    std::vector<uint8_t> json;
    auto raw = [&](const char* s) { json.insert(json.end(), s, s + std::strlen(s)); };
    char num[48];
    raw(R"({"ticket":)"); std::snprintf(num, sizeof(num), "%llu", (unsigned long long)ticket); raw(num);
    raw(R"(,"ts_ns":)"); std::snprintf(num, sizeof(num), "%lld", (long long)wall_ns); raw(num);
    raw(R"(,"mono_ns":)"); std::snprintf(num, sizeof(num), "%lld", (long long)mono_ns); raw(num);
    raw(R"(,"tid":)"); std::snprintf(num, sizeof(num), "%llu", (unsigned long long)tid); raw(num);
    raw(R"(,"level":")"); raw(level_name(level)); raw(R"(")");
    raw(R"(,"category":")"); put_json_string(json, category, std::strlen(category)); raw("\"");
    raw(R"(,"request_id":")"); if (req_id) put_json_string(json, req_id, std::strlen(req_id)); raw(R"(")");
    raw(R"(,"msg":")"); put_json_string(json, msg, std::strlen(msg)); raw("\"");
    if (fields_json && *fields_json) { raw(R"(,"fields":{)"); raw(fields_json); raw("}"); }
    raw("}");

    std::vector<uint8_t> rec;
    put_u16(rec, 1);
    put_u32(rec, seq);
    put_i64(rec, wall_ns);  // log_time: wall clock for human readers
    put_i64(rec, mono_ns);  // publish_time: monotonic clock, used by index
    rec.insert(rec.end(), json.begin(), json.end());

    uint8_t hdr[9]; hdr[0] = OP_MESSAGE;
    uint64_t len = rec.size();
    std::memcpy(hdr + 1, &len, 8);
    out.insert(out.end(), hdr, hdr + 9);
    out.insert(out.end(), rec.begin(), rec.end());
}

void McapWriter::write_log(uint32_t seq, uint16_t level, uint64_t ticket, uint64_t tid,
                           int64_t wall_ns, int64_t mono_ns,
                           const char* category, const char* req_id, const char* msg,
                           const char* fields_json) {
    (void)seq_cursor_;
    append_message(chunk_, seq, level, ticket, tid, wall_ns, mono_ns,
                   category, req_id, msg, fields_json);
    if (!cur_has_span_) {
        cur_mono_start_ = cur_mono_end_ = mono_ns;
        cur_has_span_ = true;
    } else {
        if (mono_ns < cur_mono_start_) cur_mono_start_ = mono_ns;
        if (mono_ns > cur_mono_end_) cur_mono_end_ = mono_ns;
    }
    if (chunk_.size() >= target_bytes_) flush_chunk();
}

void McapWriter::write_log_raw(uint32_t seq, int64_t wall_ns, int64_t mono_ns,
                            const std::string& json_payload) {
    std::vector<uint8_t> rec;
    put_u16(rec, 1); // channel /logguard/logs
    put_u32(rec, seq);
    put_i64(rec, wall_ns);
    put_i64(rec, mono_ns);
    rec.insert(rec.end(), json_payload.begin(), json_payload.end());
    // Frame exactly like append_message: 9-byte record header inside the chunk.
    uint8_t hdr[9];
    hdr[0] = OP_MESSAGE;
    uint64_t rlen = rec.size();
    std::memcpy(hdr + 1, &rlen, 8);
    chunk_.insert(chunk_.end(), hdr, hdr + 9);
    chunk_.insert(chunk_.end(), rec.begin(), rec.end());
    if (!cur_has_span_) {
        cur_mono_start_ = cur_mono_end_ = mono_ns;
        cur_has_span_ = true;
    } else {
        if (mono_ns < cur_mono_start_) cur_mono_start_ = mono_ns;
        if (mono_ns > cur_mono_end_) cur_mono_end_ = mono_ns;
    }
    if (chunk_.size() >= target_bytes_) flush_chunk();
}

void McapWriter::write_manifest(int64_t wall_ns, const std::string& json) {
    // Channel id 2, log_time/publish_time = wall_ns, payload is the manifest
    // JSON object verbatim. Stored as a framed message and prepended to the
    // first chunk when it is flushed, so it stays the first record even when
    // the package spans several chunks.
    std::vector<uint8_t> rec;
    put_u16(rec, 2);
    put_u32(rec, 0); // sequence
    put_i64(rec, wall_ns);
    put_i64(rec, wall_ns);
    rec.insert(rec.end(), json.begin(), json.end());

    uint8_t hdr[9]; hdr[0] = OP_MESSAGE;
    uint64_t len = rec.size();
    std::memcpy(hdr + 1, &len, 8);
    manifest_rec_.insert(manifest_rec_.end(), hdr, hdr + 9);
    manifest_rec_.insert(manifest_rec_.end(), rec.begin(), rec.end());
}

void McapWriter::flush_chunk() {
    if (chunk_.empty()) return;
    FILE* fp = static_cast<FILE*>(f_);

    // The first physical chunk also carries the manifest record at its head so
    // the package remains self-describing regardless of how many chunks follow.
    std::vector<uint8_t> raw;
    raw.reserve(manifest_rec_.size() + chunk_.size());
    if (!manifest_rec_.empty())
        raw.insert(raw.end(), manifest_rec_.begin(), manifest_rec_.end());
    raw.insert(raw.end(), chunk_.begin(), chunk_.end());

    bool want_zstd = compression_ == "zstd";
    auto& codec = export_::ZstdCodec::instance();
    std::vector<uint8_t> payload;
    std::string used = "none";
    if (want_zstd && codec.available()) {
        payload = codec.compress(raw.data(), raw.size());
        if (!payload.empty()) used = "zstd";
    }
    if (used != "zstd") payload = raw;

    const uint64_t offset =
        static_cast<uint64_t>(std::ftell(fp));

    std::vector<uint8_t> body;
    put_string(body, used);                 // compression
    put_u64(body, raw.size());              // uncompressed size
    put_u32(body, 0);                       // uncompressed crc
    body.insert(body.end(), payload.begin(), payload.end());
    write_record(OP_CHUNK, body);

    if (cur_has_span_) {
        ChunkMeta m;
        m.mono_start = cur_mono_start_;
        m.mono_end = cur_mono_end_;
        m.file_offset = offset;
        m.compressed_size = 9 + body.size(); // record header + body bytes
        m.uncompressed_size = raw.size();
        m.compression = (used == "zstd") ? 1u : 0u;
        index_.push_back(m);
    }
    manifest_rec_.clear();
    chunk_.clear();
    cur_has_span_ = false;
}

void McapWriter::finish() {
    flush_chunk();

    // Trailing chunk time index (skipped by readers that lack the opcode).
    if (!index_.empty()) {
        std::vector<uint8_t> b;
        b.insert(b.end(), CHUNK_INDEX_MAGIC, CHUNK_INDEX_MAGIC + 4);
        b.push_back(CHUNK_INDEX_VERSION);
        put_u32(b, static_cast<uint32_t>(index_.size()));
        for (const auto& m : index_) {
            put_i64(b, m.mono_start);
            put_i64(b, m.mono_end);
            put_u64(b, m.file_offset);
            put_u64(b, m.compressed_size);
            put_u64(b, m.uncompressed_size);
            b.push_back(m.compression);
        }
        write_record(OP_CHUNK_INDEX, b);
    }
    {
        std::vector<uint8_t> b; put_u32(b, 0);
        write_record(OP_DATA_END, b);
    }
    {
        std::vector<uint8_t> b;
        put_u64(b, 0); put_u64(b, 0); put_u32(b, 0);
        write_record(OP_FOOTER, b);
    }
    FILE* fp = static_cast<FILE*>(f_);
    std::fwrite(MAGIC, 1, sizeof(MAGIC), fp);
    std::fflush(fp);
}

} // namespace lg
