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
    compression_ = compression;
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
    put_i64(rec, wall_ns);
    put_i64(rec, wall_ns);
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
    append_message(chunk_, seq, level, ticket, tid, wall_ns, mono_ns,
                   category, req_id, msg, fields_json);
}

void McapWriter::write_manifest(int64_t wall_ns, const std::string& json) {
    // Channel id 2, log_time/publish_time = wall_ns, payload is the manifest
    // JSON object verbatim.
    std::vector<uint8_t> rec;
    put_u16(rec, 2);
    put_u32(rec, 0); // sequence
    put_i64(rec, wall_ns);
    put_i64(rec, wall_ns);
    rec.insert(rec.end(), json.begin(), json.end());

    // Prepend manifest so it is the first message record in the chunk.
    std::vector<uint8_t> framed;
    uint8_t hdr[9]; hdr[0] = OP_MESSAGE;
    uint64_t len = rec.size();
    std::memcpy(hdr + 1, &len, 8);
    framed.insert(framed.end(), hdr, hdr + 9);
    framed.insert(framed.end(), rec.begin(), rec.end());
    chunk_.insert(chunk_.begin(), framed.begin(), framed.end());
}

void McapWriter::finish() {
    // Flush buffered messages as one Chunk (compressed if requested/available).
    if (!chunk_.empty()) {
        bool want_zstd = compression_ == "zstd";
        auto& codec = export_::ZstdCodec::instance();
        std::vector<uint8_t> payload;
        std::string used = "none";
        if (want_zstd && codec.available()) {
            payload = codec.compress(chunk_.data(), chunk_.size());
            if (!payload.empty()) used = "zstd";
        }
        if (used != "zstd") payload = chunk_;

        std::vector<uint8_t> body;
        put_string(body, used);                 // compression
        put_u64(body, chunk_.size());           // uncompressed size
        put_u32(body, 0);                       // uncompressed crc
        body.insert(body.end(), payload.begin(), payload.end());
        write_record(OP_CHUNK, body);
        chunk_.clear();
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
