#include "export/mcap_verify.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "export/zstd_codec.h"

namespace lg {
namespace export_ {

namespace {

constexpr uint8_t kMagic[9] = {0x89, 'M', 'C', 'A', 'P', 0x0D, 0x0A, 0x1A,
                               0x0A};
constexpr size_t kMagicLen = 9;

// MCAP opcodes we recognise. These match LogGuard's writer (which predates and
// diverges slightly from the spec's schema/footer opcodes); the CLI reader uses
// the same set, so this validates exactly what the toolchain produces.
constexpr uint8_t OP_HEADER = 0x01;
constexpr uint8_t OP_SCHEMA = 0x06;
constexpr uint8_t OP_CHANNEL = 0x07; // matches LogGuard's writer
constexpr uint8_t OP_MESSAGE = 0x05;
constexpr uint8_t OP_CHUNK = 0x0D;
constexpr uint8_t OP_DATA_END = 0x0F;
constexpr uint8_t OP_FOOTER = 0x0E;

// Bounds-checked cursor over a byte span. All multi-byte readers return false
// rather than reading out of bounds.
struct Cursor {
    const uint8_t* base;
    size_t size;
    size_t pos = 0;

    Cursor(const uint8_t* b, size_t n) : base(b), size(n) {}

    bool need(size_t n) const { return pos <= size && n <= size - pos; }
    bool u8(uint8_t& v) {
        if (!need(1)) return false;
        v = base[pos++];
        return true;
    }
    bool take(size_t n, const uint8_t*& p) {
        if (!need(n)) return false;
        p = base + pos;
        pos += n;
        return true;
    }
    template <typename T>
    bool le(T& v) {
        if (!need(sizeof(T))) return false;
        T x = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            x |= T(base[pos + i]) << (8 * i);
        pos += sizeof(T);
        v = x;
        return true;
    }
    bool u16(uint16_t& v) { return le(v); }
    bool u32(uint32_t& v) { return le(v); }
    bool u64(uint64_t& v) { return le(v); }

    // Read a length-prefixed string as a span (no allocation/copy).
    bool str_span(const uint8_t*& p, uint32_t& n) {
        if (!u32(n)) return false;
        if (n > size - pos) return false;
        p = base + pos;
        pos += n;
        return true;
    }
};

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
}

// Walk a record buffer (top-level file or a decompressed chunk body), updating
// counters and collecting log messages. Chunk nesting is bounded.
void walk(const uint8_t* data, size_t size, uint32_t depth,
          std::map<uint16_t, std::string>& topics, VerifyReport& rep,
          std::vector<McapVerifier::LogMessage>* out,
          std::vector<std::string>* manifests = nullptr) {
    Cursor cur(data, size);
    while (cur.need(9)) {
        const size_t rec_abs = cur.pos;
        uint8_t op = 0;
        if (!cur.u8(op)) break;
        uint64_t len64 = 0;
        if (!cur.u64(len64)) {
            rep.error("truncated record length", rec_abs);
            break;
        }
        if (len64 > McapVerifier::kMaxRecordLen) {
            rep.error("record declares implausible length", rec_abs);
            break;
        }
        const size_t len = static_cast<size_t>(len64);
        const uint8_t* body = nullptr;
        if (!cur.take(len, body)) {
            rep.error("record body overruns container", rec_abs);
            break;
        }

        switch (op) {
        case OP_HEADER: {
            rep.has_header = true;
            Cursor bc(body, len);
            const uint8_t* prof = nullptr;
            uint32_t plen = 0;
            if (bc.str_span(prof, plen)) rep.header_profile = plen;
            break;
        }
        case OP_SCHEMA: {
            ++rep.schema_count;
            break;
        }
        case OP_CHANNEL: {
            ++rep.channel_count;
            // [u16 id][u16 schema_id][str topic][str encoding][map metadata]
            Cursor bc(body, len);
            uint16_t id = 0, sid = 0;
            const uint8_t* topic = nullptr;
            uint32_t tlen = 0;
            if (bc.u16(id) && bc.u16(sid) && bc.str_span(topic, tlen))
                topics[id] = std::string(
                    reinterpret_cast<const char*>(topic), tlen);
            break;
        }
        case OP_MESSAGE: {
            // Classify the message by channel; only /logguard/logs counts as
            // a log message for message_count. The provenance manifest on
            // /logguard/manifest is tallied separately.
            uint16_t mch = (len >= 2) ? static_cast<uint16_t>(body[0] |
                                                              (body[1] << 8))
                                      : 0xFFFF;
            const auto topic_it = topics.find(mch);
            const bool is_log =
                topic_it != topics.end() &&
                topic_it->second == "/logguard/logs";
            const bool is_manifest =
                topic_it != topics.end() &&
                topic_it->second == "/logguard/manifest";
            if (is_log) ++rep.message_count;
            else if (is_manifest) ++rep.manifest_count;
            // [u16 channel][u32 sequence][u64 log_time][u64 publish_time][data]
            if (len >= 22 && (out || manifests)) {
                Cursor bc(body, len);
                uint16_t ch = 0;
                uint32_t seq = 0;
                uint64_t lt = 0, pt = 0;
                if (bc.u16(ch) && bc.u32(seq) && bc.u64(lt) && bc.u64(pt)) {
                    auto it = topics.find(ch);
                    const size_t consumed = 22;
                    if (len < consumed) break;
                    // Inside chunks topics map is shared and accumulates
                    // top-level + inner channels; classify by topic name.
                    if (it != topics.end() && it->second == "/logguard/logs") {
                        McapVerifier::LogMessage m;
                        m.log_time_ns = lt;
                        m.channel_id = ch;
                        m.json.assign(
                            reinterpret_cast<const char*>(body + consumed),
                            len - consumed);
                        if (out) out->push_back(std::move(m));
                    } else if (it != topics.end() &&
                               it->second == "/logguard/manifest" &&
                               manifests) {
                        manifests->emplace_back(
                            reinterpret_cast<const char*>(body + consumed),
                            len - consumed);
                    }
                }
            }
            break;
        }
        case OP_CHUNK: {
            ++rep.chunk_count;
            if (depth + 1 > McapVerifier::kMaxNesting) {
                rep.warn("chunk nesting limit reached; skipped", rec_abs);
                break;
            }
            // [str compression][u64 uncompressed_size][u32 uncompressed_crc]
            // [records...]
            Cursor bc(body, len);
            const uint8_t* algo = nullptr;
            uint32_t alen = 0;
            uint64_t usize = 0;
            uint32_t crc = 0;
            if (!bc.str_span(algo, alen) || !bc.u64(usize) || !bc.u32(crc)) {
                rep.warn("malformed chunk header", rec_abs);
                ++rep.bad_chunks;
                break;
            }
            if (usize > McapVerifier::kMaxChunkUncompressed) {
                rep.warn("chunk declares implausible uncompressed size",
                         rec_abs);
                ++rep.bad_chunks;
                break;
            }
            const size_t payload_off = bc.pos;
            if (payload_off > len) {
                rep.warn("chunk header overruns body", rec_abs);
                ++rep.bad_chunks;
                break;
            }
            const uint8_t* payload = body + payload_off;
            const size_t plen = len - payload_off;
            std::vector<uint8_t> inflated;
            std::string alg(reinterpret_cast<const char*>(algo), alen);
            if (alg == "zstd") {
                inflated = ZstdCodec::instance().decompress(
                    payload, plen, static_cast<size_t>(usize));
                if (inflated.empty()) {
                    rep.warn("zstd chunk could not be decompressed", rec_abs);
                    ++rep.bad_chunks;
                    break;
                }
                walk(inflated.data(), inflated.size(), depth + 1, topics, rep,
                     out, manifests);
            } else if (alg.empty() || alg == "none") {
                walk(payload, plen, depth + 1, topics, rep, out, manifests);
            } else {
                rep.warn("unknown chunk compression; skipped", rec_abs);
                ++rep.bad_chunks;
            }
            break;
        }
        case OP_DATA_END:
            rep.has_data_end = true;
            break;
        case OP_FOOTER:
            rep.has_footer = true;
            rep.parsed_bytes = cur.pos;
            return;
        default:
            // Unknown opcode: its body length was already validated; skip.
            break;
        }
    }
    rep.parsed_bytes = cur.pos;
}

} // namespace

bool VerifyReport::has_error() const {
    for (const auto& i : issues)
        if (i.severity == VerifyIssue::Severity::Error) return true;
    return false;
}

std::string VerifyReport::render() const {
    std::ostringstream os;
    os << (ok ? "MCAP OK" : "MCAP INVALID") << ": schemas=" << schema_count
       << " channels=" << channel_count << " messages=" << message_count
       << " chunks=" << chunk_count << " bad_chunks=" << bad_chunks
       << " parsed=" << parsed_bytes << "B";
    for (const auto& i : issues) {
        os << "\n  " << (i.severity == VerifyIssue::Severity::Error ? "ERROR"
                                                                      : "warn")
           << " @" << i.offset << ": " << i.message;
    }
    return os.str();
}

VerifyReport McapVerifier::verify(const uint8_t* data, size_t size,
                                  bool allow_trailing) {
    VerifyReport rep;
    if (size < kMagicLen || std::memcmp(data, kMagic, kMagicLen) != 0) {
        rep.error("missing MCAP magic", 0);
        rep.ok = false;
        return rep;
    }
    std::map<uint16_t, std::string> topics;
    walk(data + kMagicLen, size - kMagicLen, 0, topics, rep, nullptr);
    // Writer emits a trailing magic after Footer; that is expected, not junk.
    (void)allow_trailing;
    rep.ok = !rep.has_error() && rep.has_footer;
    return rep;
}

VerifyReport McapVerifier::verify_file(const std::string& path,
                                       bool allow_trailing) {
    std::vector<uint8_t> buf;
    if (!read_file_bytes(path, buf)) {
        VerifyReport rep;
        rep.error("cannot open file", 0);
        return rep;
    }
    return verify(buf.data(), buf.size(), allow_trailing);
}

bool McapVerifier::extract_logs(const uint8_t* data, size_t size,
                                std::vector<LogMessage>& out,
                                VerifyReport* report) {
    VerifyReport local;
    VerifyReport& rep = report ? *report : local;
    if (size < kMagicLen || std::memcmp(data, kMagic, kMagicLen) != 0) {
        rep.error("missing MCAP magic", 0);
        return false;
    }
    std::map<uint16_t, std::string> topics;
    walk(data + kMagicLen, size - kMagicLen, 0, topics, rep, &out);
    rep.ok = !rep.has_error() && rep.has_footer;
    return !rep.has_error();
}

bool McapVerifier::extract_logs_file(const std::string& path,
                                     std::vector<LogMessage>& out,
                                     VerifyReport* report) {
    std::vector<uint8_t> buf;
    if (!read_file_bytes(path, buf)) {
        if (report) report->error("cannot open file", 0);
        return false;
    }
    return extract_logs(buf.data(), buf.size(), out, report);
}

bool McapVerifier::extract_manifest_file(const std::string& path,
                                         std::string& json_out) {
    std::vector<uint8_t> buf;
    if (!read_file_bytes(path, buf)) return false;
    if (buf.size() < kMagicLen ||
        std::memcmp(buf.data(), kMagic, kMagicLen) != 0)
        return false;
    VerifyReport rep;
    std::map<uint16_t, std::string> topics;
    std::vector<std::string> manifests;
    walk(buf.data() + kMagicLen, buf.size() - kMagicLen, 0, topics, rep, nullptr,
         &manifests);
    if (manifests.empty()) return false;
    json_out = std::move(manifests.back());
    return true;
}

} // namespace export_
} // namespace lg
