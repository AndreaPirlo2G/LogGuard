// lgtool <command> [options]
//   recover <ring>                       export an unclean ring
//   info <file.lgp>                      record counts / levels / span
//   cat <file.lgp>                       print records (JSON lines)
//   query <file.lgp> [needle]            substring filter over raw JSON
//   report <file.lgp> [out.html]         self-contained HTML incident report
//   fingerprint <file.lgp|file.fp>       error signatures + counts
//   diff <baseline.fp|lgp> <candidate>   added / removed / changed signatures
//   tail <ring>                          live view of a running ring
//   scan <ring> [level>=X] [cat=C] [req=R] [tid=T] [text=..]
//                                        structured filter over a live ring
#include <logguard/logguard.h>
#include "core/lazy_format.h"
#include "export/zstd_codec.h"
#include "export/mcap_verify.hpp"
#include "core/ring_audit.hpp"
#include "core/ring_reader.hpp"
#include "export/query.hpp"
#include "export/retention.hpp"
#include "platform/clock.hpp"
#include "diagnosis/session_stats.hpp"
#include "diagnosis/fingerprint.hpp"
#include "diagnosis/signature_diff.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace diag = lg::diag;

static uint16_t rd_u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1] << 8); }
static uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint64_t rd_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

static const uint8_t MCAP_MAGIC[9] = {0x89, 'M', 'C', 'A', 'P', '\r', '\n', 0x1A, '\n'};
constexpr uint32_t SLOT_MAGIC_R = 0x3153474Cu; // keep in sync with core/constants.hpp
constexpr size_t   SLOT_SIZE_R  = 256;
constexpr size_t   HEADER_SIZE_R = 4096;
constexpr size_t   CATEGORY_CAP_R = 16;
constexpr size_t   REQ_CAP_R = 24;
constexpr size_t   PAYLOAD_CAP_R = 166;
constexpr uint16_t FLAG_STACK_R = 1;
constexpr uint16_t FLAG_KV_R = 2;

#pragma pack(push, 1)
struct RingSlot {
    uint32_t magic; uint32_t epoch; uint64_t ticket; int64_t mono_ns; int64_t wall_ns;
    uint64_t tid; uint16_t level, flags, msg_len, cat_len, req_len;
    char category[CATEGORY_CAP_R]; char req_id[REQ_CAP_R]; char payload[PAYLOAD_CAP_R];
};
struct RingHeader {
    char magic[8]; uint32_t version, slot_size; uint64_t slot_count;
    int32_t pid; uint32_t epoch, clean, crashed; int32_t sig;
    uint64_t fault_addr; int64_t crash_mono_ns; uint64_t next_ticket;
};
#pragma pack(pop)

struct Rec { uint64_t ts = 0; std::string json; };

// Parse Message/Channel records out of a flat record buffer (top-level file or
// a decompressed Chunk body).
static void parse_records(const std::vector<uint8_t>& buf,
                          std::map<uint16_t, std::string>& topics,
                          std::vector<Rec>& out) {
    size_t pos = 0;
    while (pos + 9 <= buf.size()) {
        uint8_t op = buf[pos];
        uint64_t len = rd_u64(&buf[pos + 1]);
        pos += 9;
        if (pos + len > buf.size()) break;
        const uint8_t* c = &buf[pos];
        if (op == 0x07 && len >= 8) { // Channel
            uint16_t id = rd_u16(c);
            uint32_t slen = rd_u32(c + 4);
            if (size_t(8) + slen <= len)
                topics[id] = std::string(reinterpret_cast<const char*>(c + 8), slen);
        } else if (op == 0x05 && len >= 22) { // Message
            uint16_t ch = rd_u16(c);
            auto it = topics.find(ch);
            if (it != topics.end() && it->second == "/logguard/logs") {
                Rec r;
                r.ts = rd_u64(c + 6);
                r.json.assign(reinterpret_cast<const char*>(c + 22), size_t(len) - 22);
                out.push_back(std::move(r));
            }
        }
        pos += size_t(len);
    }
}

static std::vector<Rec> read_mcap(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    std::vector<Rec> out;
    if (buf.size() < 18 || std::memcmp(buf.data(), MCAP_MAGIC, 9) != 0) {
        fprintf(stderr, "not an MCAP file: %s\n", path.c_str());
        return out;
    }
    std::map<uint16_t, std::string> topics;
    // Pass 1 (top level, outside chunks) collects channels and plain messages.
    size_t pos = 9;
    while (pos + 9 <= buf.size()) {
        uint8_t op = buf[pos];
        uint64_t len = rd_u64(&buf[pos + 1]);
        size_t rec_start = pos + 9;
        pos = rec_start;
        if (pos + len > buf.size()) break;
        const uint8_t* c = &buf[pos];
        if (op == 0x0E) break; // Footer
        if (op == 0x07 && len >= 8) { // Channel (top-level body, header stripped)
            uint16_t id = rd_u16(c);
            uint32_t slen = rd_u32(c + 4);
            if (size_t(8) + slen <= len)
                topics[id] = std::string(reinterpret_cast<const char*>(c + 8), slen);
        } else if (op == 0x05 && len >= 22) { // Message (top-level)
            uint16_t ch = rd_u16(c);
            auto it = topics.find(ch);
            if (it != topics.end() && it->second == "/logguard/logs") {
                Rec r;
                r.ts = rd_u64(c + 6);
                r.json.assign(reinterpret_cast<const char*>(c + 22), size_t(len) - 22);
                out.push_back(std::move(r));
            }
        } else if (op == 0x0D && len >= 13) { // Chunk: [str algo][u64 usize][u32 crc][records...]
            uint32_t alen = rd_u32(c);
            std::string algo(reinterpret_cast<const char*>(c + 4), alen);
            size_t off = 4 + alen;
            uint64_t usize = rd_u64(c + off); off += 8;
            off += 4; // uncompressed crc
            std::vector<uint8_t> body;
            if (off <= len) {
                const uint8_t* pdata = c + off;
                size_t plen = size_t(len) - off;
                if (algo == "zstd") {
                    body = lg::export_::ZstdCodec::instance()
                               .decompress(pdata, plen, size_t(usize));
                    if (body.empty())
                        fprintf(stderr, "warning: cannot decompress zstd chunk "
                                        "(libzstd unavailable?)\n");
                } else {
                    body.assign(pdata, pdata + plen);
                }
            }
            if (!body.empty()) parse_records(body, topics, out);
        }
        pos += size_t(len);
    }
    return out;
}

static std::string json_field(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    std::string v;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) { v += j[p + 1]; p += 2; }
        else v += j[p++];
    }
    return v;
}

static int cmd_cat(const std::string& path) {
    auto recs = read_mcap(path);
    for (auto& r : recs) printf("%llu %s\n", (unsigned long long)r.ts, r.json.c_str());
    fprintf(stderr, "%zu records\n", recs.size());
    return 0;
}

static int cmd_query(const std::string& path, const std::string& needle) {
    auto recs = read_mcap(path);
    size_t n = 0;
    for (auto& r : recs)
        if (needle.empty() || r.json.find(needle) != std::string::npos) {
            printf("%s\n", r.json.c_str());
            ++n;
        }
    fprintf(stderr, "%zu/%zu matched\n", n, recs.size());
    return n ? 0 : 1;
}

static int cmd_info(const std::string& path) {
    auto recs = read_mcap(path);
    std::map<std::string, int> levels, cats;
    for (auto& r : recs) {
        levels[json_field(r.json, "level")]++;
        cats[json_field(r.json, "category")]++;
    }
    printf("file:       %s\n", path.c_str());
    printf("records:    %zu\n", recs.size());
    if (!recs.empty()) {
        printf("first ts:   %llu\n", (unsigned long long)recs.front().ts);
        printf("last ts:    %llu\n", (unsigned long long)recs.back().ts);
    }
    printf("levels:");
    for (auto& [k, v] : levels) printf(" %s=%d", k.c_str(), v);
    printf("\ncategories:");
    for (auto& [k, v] : cats) printf(" %s=%d", k.c_str(), v);
    printf("\n");
    return 0;
}

static void html_escape(std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
            case '&': o += "&amp;"; break; default: o += c;
        }
    }
    s.swap(o);
}

static int cmd_report(const std::string& path, const std::string& out) {
    auto recs = read_mcap(path);
    std::map<std::string, int> levels;
    for (auto& r : recs) levels[json_field(r.json, "level")]++;
    FILE* fp = std::fopen(out.c_str(), "w");
    if (!fp) { perror("open report"); return 1; }
    std::fputs("<!doctype html><meta charset=utf-8><title>LogGuard report</title>"
               "<style>body{font:14px/1.5 ui-monospace,Menlo,monospace;margin:24px;color:#1a1a1a}"
               "h1{font-size:18px}.m{padding:4px 8px;border-bottom:1px solid #eee;white-space:pre-wrap}"
               ".CRITICAL{background:#fee}.ERROR{background:#fff5f5}.WARN{background:#fffbeb}"
               ".tag{display:inline-block;min-width:72px;color:#666}.c{color:#0a6}</style>", fp);
    std::fprintf(fp, "<h1>LogGuard incident report</h1><div>source: %s &middot; %zu records</div><div>",
                 path.c_str(), recs.size());
    for (auto& [k, v] : levels) std::fprintf(fp, " %s=%d", k.c_str(), v);
    std::fputs("</div><br>", fp);
    for (auto& r : recs) {
        std::string lvl = json_field(r.json, "level");
        std::string cat = json_field(r.json, "category");
        std::string req = json_field(r.json, "request_id");
        std::string msg = json_field(r.json, "msg");
        html_escape(msg);
        std::fprintf(fp, "<div class=m %s><span class=tag>%s</span><b>%s</b>",
                     lvl.c_str(), lvl.c_str(), cat.c_str());
        if (!req.empty()) std::fprintf(fp, " <span class=c>%s</span>", req.c_str());
        std::fprintf(fp, " %s</div>", msg.c_str());
    }
    std::fclose(fp);
    printf("wrote %s (%zu records)\n", out.c_str(), recs.size());
    return 0;
}

// ---------------- fingerprint & diff ----------------
// The normalization/comparison logic lives in the reusable diag library; the
// CLI only builds a SignatureSet from a .lgp/.fp path and prints results.

static diag::SignatureSet fingerprints_of(const std::string& path) {
    diag::SignatureSet set;
    if (path.size() > 4 && path.substr(path.size() - 4) == ".lgp") {
        for (const auto& r : read_mcap(path)) {
            diag::add_observation(set, json_field(r.json, "level"),
                                  json_field(r.json, "category"),
                                  json_field(r.json, "msg"));
        }
    } else {
        std::ifstream f(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        set = diag::deserialize(text);
    }
    return set;
}

static int cmd_fingerprint(const std::string& path) {
    diag::SignatureSet set = fingerprints_of(path);
    fputs(diag::serialize(set).c_str(), stdout);
    fprintf(stderr, "%zu distinct signatures\n", set.size());
    return 0;
}

static int cmd_diff(const std::string& base, const std::string& cand) {
    diag::DiffReport rep = diag::compare(fingerprints_of(base),
                                         fingerprints_of(cand));
    fputs(diag::render(rep).c_str(), stdout);
    return rep.has_regression() ? 1 : 0;
}

// ---------------- tail ----------------

static const char* lvl_name(uint16_t l) {
    static const char* n[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"};
    return l <= 5 ? n[l] : "?";
}

static void emit_slot(const RingSlot* s) {
    std::string msg;
    std::string fields;
    if (s->flags & FLAG_KV_R) {
        size_t z;
        for (z = 0; z < s->msg_len && z < PAYLOAD_CAP_R && s->payload[z]; ++z) {}
        msg.assign(s->payload, z);
        if (z < s->msg_len) fields.assign(s->payload + z + 1, s->msg_len - z - 1);
    } else if (s->flags & FLAG_STACK_R) {
        msg = "<thread stack>";
    } else {
        msg.assign(s->payload, s->msg_len);
    }
    std::string cat(s->category, s->cat_len);
    std::string req(s->req_id, s->req_len);
    printf("%llu [%s] %s", (unsigned long long)s->ticket, lvl_name(s->level), cat.c_str());
    if (!req.empty()) printf(" {%s}", req.c_str());
    printf(" %s", msg.c_str());
    if (!fields.empty()) printf(" {%s}", fields.c_str());
    putchar('\n');
    fflush(stdout);
}

static volatile sig_atomic_t g_stop = 0;
static void on_term(int) { g_stop = 1; }

static int cmd_scan(const std::string& ring_path,
                    const std::string& expr) {
    int fd = open(ring_path.c_str(), O_RDONLY);
    if (fd < 0) { perror("open ring"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0 ||
        st.st_size < (off_t)(HEADER_SIZE_R + SLOT_SIZE_R)) {
        fprintf(stderr, "ring too small or missing: %s\n", ring_path.c_str());
        close(fd);
        return 1;
    }
    void* base = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    auto* h = static_cast<const RingHeader*>(base);

    lg::export_::QuerySpec spec;
    std::string err;
    if (!expr.empty() && !lg::export_::parse_query(expr, spec, &err)) {
        fprintf(stderr, "bad query: %s\n", err.c_str());
        munmap(base, st.st_size);
        return 2;
    }

    std::vector<lg::core::Slot> all =
        lg::core::collect_epoch(base, h->slot_count, h->epoch);
    auto hits = lg::export_::query(all, spec);
    for (const auto& hit : hits)
        printf("%s\n", lg::export_::render_hit(hit).c_str());
    fprintf(stderr, "%zu/%zu matched\n", hits.size(), all.size());
    munmap(base, st.st_size);
    return hits.empty() ? 1 : 0;
}

static int cmd_tail(const std::string& ring_path) {
    int fd = open(ring_path.c_str(), O_RDONLY);
    if (fd < 0) { perror("open ring"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(HEADER_SIZE_R + SLOT_SIZE_R)) {
        fprintf(stderr, "ring too small or missing: %s\n", ring_path.c_str());
        return 1;
    }
    void* base = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    auto* h = static_cast<const RingHeader*>(base);
    auto* slots = reinterpret_cast<const RingSlot*>(static_cast<const char*>(base) + HEADER_SIZE_R);
    uint64_t mask = h->slot_count - 1;

    signal(SIGINT, on_term); signal(SIGTERM, on_term);
    uint64_t seen = h->next_ticket; // start at current tip; show only new records
    fprintf(stderr, "tailing %s (%llu slots), ctrl-c to stop\n",
            ring_path.c_str(), (unsigned long long)h->slot_count);
    while (!g_stop) {
        uint64_t tip = __atomic_load_n(&h->next_ticket, __ATOMIC_ACQUIRE);
        while (seen < tip) {
            const RingSlot* s = slots + (seen & mask);
            uint32_t magic = __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE);
            if (magic == SLOT_MAGIC_R && s->epoch == h->epoch) emit_slot(s);
            ++seen;
        }
        usleep(100 * 1000);
    }
    munmap(base, st.st_size);
    close(fd);
    return 0;
}

static int cmd_audit(const std::string& ring_path) {
    int fd = open(ring_path.c_str(), O_RDONLY);
    if (fd < 0) { perror("open ring"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(HEADER_SIZE_R + SLOT_SIZE_R)) {
        fprintf(stderr, "ring too small or missing: %s\n", ring_path.c_str());
        close(fd);
        return 1;
    }
    void* base = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    uint64_t cap = (uint64_t(st.st_size) - HEADER_SIZE_R) / SLOT_SIZE_R;
    lg::core::RingAudit a = lg::core::audit_ring(base, cap);
    // Snapshot current-epoch slots for the session summary while mapped.
    std::vector<lg::core::Slot> live;
    if (a.header_ok)
        live = lg::core::collect_epoch(base, a.capacity, a.header_epoch);
    munmap(base, st.st_size);
    close(fd);

    printf("ring: %s\n", ring_path.c_str());
    printf("  header_ok      : %s\n", a.header_ok ? "yes" : "NO");
    printf("  epoch          : %u\n", a.header_epoch);
    printf("  crashed flag   : %s\n", a.crashed ? "yes" : "no");
    printf("  capacity       : %llu\n", (unsigned long long)a.capacity);
    printf("  valid          : %llu (%.1f%% full)\n",
           (unsigned long long)a.valid, a.fill_ratio() * 100.0);
    printf("  empty          : %llu\n", (unsigned long long)a.empty);
    printf("  foreign epoch  : %llu\n", (unsigned long long)a.foreign_epoch);
    printf("  torn slots     : %llu\n", (unsigned long long)a.torn);
    printf("  ticket mismatch: %llu\n", (unsigned long long)a.ticket_mismatch);
    if (a.distinct_tickets)
        printf("  ticket span    : %llu..%llu (%llu distinct)\n",
               (unsigned long long)a.min_ticket,
               (unsigned long long)a.max_ticket,
               (unsigned long long)a.distinct_tickets);
    printf("  status         : %s\n",
           a.healthy() ? "HEALTHY" : "DEGRADED");
    if (!live.empty()) {
        lg::diag::SessionStats stats = lg::diag::aggregate(live);
        printf("---- current epoch session ----\n");
        printf("%s", lg::diag::render_stats(stats).c_str());
    }
    return a.healthy() ? 0 : 1;
}

static int cmd_verify(const std::string& path) {
    auto rep = lg::export_::McapVerifier::verify_file(path, /*trailing*/ true);
    fputs(rep.render().c_str(), stdout);
    putchar('\n');
    return rep.ok ? 0 : 1;
}

// Pretty-print a compact JSON object with two-space indentation. Returns the
// input unchanged if it does not look like a JSON object/array.
static std::string pretty_json(const std::string& j) {
    std::string out;
    out.reserve(j.size() + j.size() / 4);
    int depth = 0;
    bool in_str = false, esc = false;
    for (char c : j) {
        if (in_str) {
            out.push_back(c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        switch (c) {
        case '"': in_str = true; out.push_back(c); break;
        case '{': case '[':
            ++depth; out.push_back(c); out.push_back('\n');
            out.append(depth * 2, ' '); break;
        case '}': case ']':
            --depth; out.push_back('\n'); out.append(depth * 2, ' ');
            out.push_back(c); break;
        case ',': out.push_back(c); out.push_back('\n');
                  out.append(depth * 2, ' '); break;
        case ':': out += ": "; break;
        default: out.push_back(c);
        }
    }
    return out;
}

static int cmd_manifest(const std::string& path) {
    std::string json;
    if (!lg::export_::McapVerifier::extract_manifest_file(path, json)) {
        fprintf(stderr, "no manifest channel in %s\n", path.c_str());
        return 1;
    }
    fputs(pretty_json(json).c_str(), stdout);
    putchar('\n');
    return 0;
}

// prune <dir> [--files N] [--mb N] [--age-hours H] [--dry-run]
static int cmd_prune(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: lgtool prune <dir> [--files N] [--mb N] "
                        "[--age-hours H] [--dry-run]\n");
        return 2;
    }
    std::string dir = argv[2];
    lg::export_::RetentionPolicy pol;
    bool dry = false;
    for (int i = 3; i + 1 < argc; ++i) {
        std::string f = argv[i];
        long long v = std::strtoll(argv[i + 1], nullptr, 10);
        if (f == "--files") pol.max_files = uint32_t(v > 0 ? v : 1);
        else if (f == "--mb") pol.max_total_bytes =
                 uint64_t(v > 0 ? v : 0) * 1024ull * 1024ull;
        else if (f == "--age-hours")
            pol.max_age_ns = int64_t(v > 0 ? v : 0) * 3600ull * 1000000000ull;
        else if (f == "--dry-run") { dry = true; continue; }
        else { fprintf(stderr, "unknown flag: %s\n", f.c_str()); return 2; }
        ++i;
    }
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--dry-run") dry = true;

    bool found = false;
    auto files = lg::export_::enumerate_packages(dir, &found);
    if (!found) {
        fprintf(stderr, "no such directory or no .lgp packages: %s\n",
                dir.c_str());
        return 1;
    }
    int64_t now = lg::plat::wall_now_ns();
    uint64_t total = 0;
    for (const auto& f : files) total += f.size_bytes;
    printf("found %zu package(s), %.2f MiB total\n", files.size(),
           total / (1048576.0));

    if (dry) {
        // Reuse the pure policy on an in-memory list; nothing is removed.
        auto plan = lg::export_::apply_retention(files, pol, now);
        for (const auto& p : plan.deleted) printf("would delete %s\n", p.c_str());
        printf("dry-run: would remove %zu package(s), free %.2f MiB; "
               "keep %u\n",
               plan.deleted.size(), plan.bytes_freed / 1048576.0,
               plan.files_kept);
        return 0;
    }
    auto res = lg::export_::enforce_retention(dir, pol, now);
    for (const auto& p : res.deleted) printf("deleted %s\n", p.c_str());
    printf("removed %zu package(s), freed %.2f MiB; kept %u (%.2f MiB)\n",
           res.deleted.size(), res.bytes_freed / 1048576.0, res.files_kept,
           res.bytes_kept / 1048576.0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage:\n"
                        "  lgtool recover <ring>\n"
                        "  lgtool info|cat <file.lgp>\n"
                        "  lgtool query <file.lgp> [needle]\n"
                        "  lgtool report <file.lgp> [out.html]\n"
                        "  lgtool fingerprint <file.lgp|file.fp>\n"
                        "  lgtool diff <baseline.fp|lgp> <candidate.fp|lgp>\n"
                        "  lgtool tail <ring>\n"
                        "  lgtool scan <ring> [level>=WARN] [cat=C] [req=R] [tid=T] [text=..]\n"
                        "  lgtool verify <file.lgp>\n"
                        "  lgtool manifest <file.lgp>\n"
                        "  lgtool audit <ring>\n"
                        "  lgtool prune <dir> [--files N] [--mb N] [--age-hours H] [--dry-run]\n");
        return 2;
    }
    std::string cmd = argv[1], path = argv[2];
    if (cmd == "recover") return lg_recover_export(path.c_str());
    if (cmd == "cat") return cmd_cat(path);
    if (cmd == "info") return cmd_info(path);
    if (cmd == "query") return cmd_query(path, argc >= 4 ? argv[3] : "");
    if (cmd == "report") return cmd_report(path, argc >= 4 ? argv[3] : "report.html");
    if (cmd == "fingerprint") return cmd_fingerprint(path);
    if (cmd == "diff") return argc >= 4 ? cmd_diff(path, argv[3]) : 2;
    if (cmd == "tail") return cmd_tail(path);
    if (cmd == "scan") {
        std::string expr;
        for (int i = 3; i < argc; ++i) { if (i > 3) expr += ' '; expr += argv[i]; }
        return cmd_scan(path, expr);
    }
    if (cmd == "verify") return cmd_verify(path);
    if (cmd == "manifest") return cmd_manifest(path);
    if (cmd == "audit") return cmd_audit(path);
    if (cmd == "prune") return cmd_prune(argc, argv);
    fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
}
