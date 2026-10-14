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
constexpr uint8_t OP_CHUNK_INDEX = 0x40; // LogGuard-specific summary section

const uint8_t kChunkIndexMagic[4] = {'L', 'G', 'I', 'X'};
constexpr uint8_t kChunkIndexVersion = 1;
constexpr uint32_t kMaxChunkIndexEntries = 1u << 20; // 1 M chunks per package

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
    bool i64(int64_t& v) {
        uint64_t raw = 0;
        if (!le(raw)) return false;
        std::memcpy(&v, &raw, sizeof(v));
        return true;
    }

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
          std::vector<std::string>* manifests = nullptr,
          bool metadata_only = false) {
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
                        m.mono_time_ns = static_cast<int64_t>(pt);
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
            if (metadata_only) break; // only gathering top-level channel map
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
        case OP_CHUNK_INDEX: {
            // Validate the section structurally (the hardening target) but do
            // not need its entries during a plain verify.
            Cursor bc(body, len);
            const uint8_t* mg = nullptr;
            if (!bc.take(4, mg) ||
                std::memcmp(mg, kChunkIndexMagic, 4) != 0) {
                rep.warn("malformed chunk index magic", rec_abs);
                break;
            }
            uint8_t ver = 0;
            uint32_t cnt = 0;
            if (!bc.u8(ver) || !bc.u32(cnt)) {
                rep.warn("truncated chunk index header", rec_abs);
                break;
            }
            if (ver != kChunkIndexVersion || cnt > kMaxChunkIndexEntries) {
                rep.warn("unsupported chunk index version/count", rec_abs);
                break;
            }
            // 33 bytes per entry (i64+i64+u64+u64+u64+u8).
            if (!bc.need(size_t(cnt) * 33)) {
                rep.warn("chunk index overruns record body", rec_abs);
                break;
            }
            rep.has_chunk_index = true;
            rep.indexed_chunks = cnt;
            break;
        }
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

bool McapVerifier::read_chunk_index(const uint8_t* data, size_t size,
                                    std::vector<ChunkIndexEntry>& out,
                                    VerifyReport* report) {
    out.clear();
    if (!data || size < kMagicLen + 1) return false;
    if (std::memcmp(data, kMagic, kMagicLen) != 0) return false;

    const uint8_t* p = data + kMagicLen;
    const size_t begin = kMagicLen;
    size_t off = begin;
    while (off + 9 <= size) {
        const uint8_t opcode = data[off];
        uint64_t rlen64 = 0;
        std::memcpy(&rlen64, data + off + 1, 8);
        if (rlen64 > McapVerifier::kMaxRecordLen) break;
        const size_t rlen = static_cast<size_t>(rlen64);
        // Overflow-safe: compare within [off,size] instead of adding pointers.
        if (rlen > size - (off + 9)) break; // footer/closing magic in the tail
        const uint8_t* body = data + off + 9;
        if (opcode == OP_CHUNK_INDEX) {
            Cursor c(body, rlen);
            const uint8_t* mg = nullptr;
            uint8_t ver = 0;
            uint32_t cnt = 0;
            if (!c.take(4, mg) ||
                std::memcmp(mg, kChunkIndexMagic, 4) != 0 ||
                !c.u8(ver) || !c.u32(cnt) || ver != kChunkIndexVersion ||
                cnt > kMaxChunkIndexEntries ||
                !c.need(size_t(cnt) * 33)) {
                if (report) report->warn("invalid chunk index section");
                return false;
            }
            out.reserve(cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                ChunkIndexEntry e;
                if (!c.i64(e.mono_start_ns) || !c.i64(e.mono_end_ns) ||
                    !c.u64(e.file_offset) || !c.u64(e.compressed_size) ||
                    !c.u64(e.uncompressed_size))
                    return false;
                uint8_t flag = 0;
                if (!c.u8(flag)) return false;
                e.zstd = flag != 0;
                out.push_back(e);
            }
            return true;
        }
        off += 9 + rlen;
    }
    return false;
}

bool McapVerifier::read_chunk_index_file(const std::string& path,
                                         std::vector<ChunkIndexEntry>& out) {
    std::vector<uint8_t> buf;
    if (!read_file_bytes(path, buf)) return false;
    return read_chunk_index(buf.data(), buf.size(), out);
}

bool McapVerifier::extract_range_file_blob(const std::vector<uint8_t>& buf,
                                           int64_t from_mono_ns,
                                           int64_t to_mono_ns,
                                           std::vector<LogMessage>& out) {
    out.clear();
    if (buf.size() < kMagicLen ||
        std::memcmp(buf.data(), kMagic, kMagicLen) != 0)
        return false;

    std::vector<ChunkIndexEntry> idx;
    if (!read_chunk_index(buf.data(), buf.size(), idx) || idx.empty()) {
        // Older single-chunk package: fall back to a linear walk.
        VerifyReport rep;
        std::vector<LogMessage> all;
        if (!extract_logs(buf.data(), buf.size(), all, &rep)) return false;
        for (auto& m : all) {
            if (m.mono_time_ns >= from_mono_ns &&
                m.mono_time_ns <= to_mono_ns)
                out.push_back(std::move(m));
        }
        return true;
    }

    // Chunks are written in monotonic order; binary-search the first chunk
    // whose end >= from, then walk while start <= to.
    auto end_ge = [&](size_t i) { return idx[i].mono_end_ns >= from_mono_ns; };
    size_t lo = 0, hi = idx.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (end_ge(mid))
            hi = mid;
        else
            lo = mid + 1;
    }
    const size_t start_chunk = lo;

    // One cheap top-level pass collects the channel-id -> topic map without
    // inflating any chunk; channels live outside the chunks.
    std::map<uint16_t, std::string> topics;
    VerifyReport meta;
    walk(buf.data() + kMagicLen, buf.size() - kMagicLen, 0, topics, meta,
         nullptr, nullptr, /*metadata_only=*/true);

    for (size_t i = start_chunk; i < idx.size() &&
                                idx[i].mono_start_ns <= to_mono_ns;
         ++i) {
        const ChunkIndexEntry& e = idx[i];
        if (e.file_offset >= buf.size()) continue;
        const size_t fo = static_cast<size_t>(e.file_offset);
        const uint8_t* rec = buf.data() + fo;
        if (fo + 9 > buf.size()) continue;
        if (rec[0] != OP_CHUNK) continue;
        uint64_t rlen64 = 0;
        std::memcpy(&rlen64, rec + 1, 8);
        if (rlen64 > McapVerifier::kMaxRecordLen) continue;
        const size_t rlen = static_cast<size_t>(rlen64);
        if (rlen > buf.size() - (fo + 9)) continue;
        const uint8_t* body = rec + 9;
        // Chunk header: [str compression][u64 uncompressed_size][u32 crc].
        Cursor hc(body, rlen);
        const uint8_t* encp = nullptr;
        uint32_t enc_len = 0;
        uint64_t usize = 0;
        uint32_t crc = 0;
        if (!hc.str_span(encp, enc_len) || !hc.u64(usize) || !hc.u32(crc))
            continue;
        const uint8_t* dptr = body + hc.pos;
        size_t dlen = rlen - hc.pos;
        std::string encoding(reinterpret_cast<const char*>(encp), enc_len);

        const uint8_t* in = nullptr;
        size_t in_len = 0;
        std::vector<uint8_t> inflated;
        if (encoding == "zstd") {
            inflated = ZstdCodec::instance().decompress(dptr, dlen,
                                                        static_cast<size_t>(usize));
            if (inflated.empty()) continue;
            in = inflated.data();
            in_len = inflated.size();
        } else {
            in = dptr;
            in_len = dlen;
        }
        VerifyReport sub;
        std::vector<LogMessage> msgs;
        walk(in, in_len, 0, topics, sub, &msgs, nullptr);
        for (auto& m : msgs) {
            if (m.mono_time_ns >= from_mono_ns &&
                m.mono_time_ns <= to_mono_ns)
                out.push_back(std::move(m));
        }
    }
    return true;
}

bool McapVerifier::extract_range_file(const std::string& path,
                                      int64_t from_mono_ns, int64_t to_mono_ns,
                                      std::vector<LogMessage>& out) {
    std::vector<uint8_t> buf;
    if (!read_file_bytes(path, buf)) return false;
    return extract_range_file_blob(buf, from_mono_ns, to_mono_ns, out);
}

} // namespace export_
} // namespace lg
