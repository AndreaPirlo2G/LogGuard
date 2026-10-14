#include "export/exporter.hpp"

#include "export/mcap_writer.h"
#include "diagnosis/symbolize.hpp"
#include "diagnosis/system_snapshot.hpp"
#include "export/redaction.hpp"
#include "export/slot_render.hpp"
#include "platform/clock.hpp"
#include "platform/file_util.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/json_util.hpp"

namespace lg::export_ {

using lg::core::Slot;
using lg::core::FileHeader;
using lg::core::RING_MAGIC_NUMBER;

std::vector<Slot>
collect_window(const Slot* slots, uint64_t slot_count, uint32_t epoch) {
    std::vector<Slot> recs;
    recs.reserve(static_cast<size_t>(std::min<uint64_t>(slot_count, 4096)));
    for (uint64_t i = 0; i < slot_count; ++i) {
        const Slot* s = slots + i;
        if (__atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != RING_MAGIC_NUMBER) continue;
        Slot c;
        std::memcpy(&c, s, sizeof(c));
        if (c.magic != RING_MAGIC_NUMBER || c.epoch != epoch) continue;
        if ((c.ticket & (slot_count - 1)) != i) continue;
        recs.push_back(c);
    }
    std::sort(recs.begin(), recs.end(),
              [](const Slot& a, const Slot& b) { return a.ticket < b.ticket; });
    return recs;
}

namespace {

// Render and write a ticket-ordered slot list, applying redaction when present.
// Returns the number of log records written.
uint32_t write_slots(lg::McapWriter& w, const std::vector<Slot>& recs,
                     const ExportOptions& opts, ExportResult& result) {
    uint32_t seq = 0;
    const Redactor* red = opts.redactor;
    RedactionStats acc;
    auto add = [&acc](const RedactionStats& s) {
        acc.phone += s.phone; acc.email += s.email; acc.id_card += s.id_card;
        acc.bank_card += s.bank_card; acc.ipv4 += s.ipv4; acc.secret += s.secret;
        acc.custom += s.custom;
    };
    for (const Slot& s : recs) {
        RenderedSlot r = render_slot(s, opts.symbolizer);
        if (red) {
            add(red->apply(r.message));
            if (!r.fields_json.empty()) add(red->apply(r.fields_json));
        }
        w.write_log(seq++, r.level, s.ticket, s.tid, s.wall_ns, s.mono_ns,
                    r.category.c_str(), r.request_id.c_str(), r.message.c_str(),
                    r.fields_json.empty() ? nullptr : r.fields_json.c_str());
    }
    result.redacted_hits += acc.total();
    result.redaction = acc;
    return seq;
}

// Build the self-describing manifest JSON written on /logguard/manifest. It
// records provenance so a package copied off a robot is still interpretable:
// producer/version, why/how it was exported, event count, compression and
// privacy/symbolization flags, and the observed mono time span.
std::string build_manifest(const std::vector<Slot>& recs,
                           const ExportOptions& opts, int extra,
                           bool crashed, const RedactionStats* rs) {
    int64_t first_mono = 0, last_mono = 0;
    if (!recs.empty()) {
        first_mono = last_mono = static_cast<int64_t>(recs.front().mono_ns);
        for (const auto& s : recs) {
            int64_t m = static_cast<int64_t>(s.mono_ns);
            if (m < first_mono) first_mono = m;
            if (m > last_mono) last_mono = m;
        }
    }
    char num[64];
    std::string j = "{";
    j += "\"producer\":\"logguard\"";
    j += ",\"version\":\"0.4.0\"";
    j += ",\"exported_at_ns\":";
    std::snprintf(num, sizeof(num), "%lld",
                  (long long)lg::plat::wall_now_ns());
    j += num;
    j += ",\"trigger\":";
    lg::core::json::write_escaped(j, opts.prefix ? opts.prefix : "export");
    j += ",\"events\":";
    std::snprintf(num, sizeof(num), "%zu", recs.size());
    j += num;
    j += ",\"extra_records\":";
    std::snprintf(num, sizeof(num), "%d", extra);
    j += num;
    j += ",\"compression\":";
    j += opts.zstd ? "\"zstd\"" : "\"none\"";
    j += ",\"redacted\":";
    j += (opts.redactor != nullptr) ? "true" : "false";
    j += ",\"redaction_report\":{";
    if (rs) {
        char r[256];
        std::snprintf(r, sizeof(r),
            "\"phone\":%d,\"email\":%d,\"id_card\":%d,\"bank_card\":%d,"
            "\"ipv4\":%d,\"secret\":%d,\"custom\":%d,\"total\":%d",
            rs->phone, rs->email, rs->id_card, rs->bank_card, rs->ipv4,
            rs->secret, rs->custom, rs->total());
        j += r;
    } else {
        j += "\"phone\":0,\"email\":0,\"id_card\":0,\"bank_card\":0,"
             "\"ipv4\":0,\"secret\":0,\"custom\":0,\"total\":0";
    }
    j += "}";
    j += ",\"symbolized\":";
    j += (opts.symbolizer != nullptr) ? "true" : "false";
    j += ",\"crashed\":";
    j += crashed ? "true" : "false";
    j += ",\"span_monotonic_ns\":";
    std::snprintf(num, sizeof(num), "%lld",
                  (long long)(last_mono - first_mono));
    j += num;
    // Failure-time host/process context (best-effort; never blocks export).
    j += ",\"system\":{";
    j += lg::diag::capture_snapshot().render_json();
    j += "}";
    j += "}";
    return j;
}

} // namespace

ExportResult
export_slots(std::vector<Slot> recs, const std::string& dir,
             const ExportOptions& opts) {
    ExportResult result;
    const int64_t wall = lg::plat::wall_now_ns();
    std::string path = lg::plat::package_path(dir, opts.prefix, wall);
    result.path = path;

    lg::McapWriter w;
    if (!w.open(path)) return result;
    w.begin(opts.zstd ? "zstd" : "none");

    uint32_t seq = write_slots(w, recs, opts, result);
    w.write_manifest(wall, build_manifest(recs, opts, 0, false,
                                          opts.redactor ? &result.redaction : nullptr));
    w.finish();
    result.count = static_cast<int>(seq);
    result.ok = true;
    return result;
}

ExportResult export_window(const FileHeader& header, const Slot* slots,
                           uint64_t slot_count, const std::string& dir,
                           const ExportOptions& opts) {
    ExportResult result;
    std::vector<Slot> recs = collect_window(slots, slot_count, header.epoch);

    const int64_t wall = lg::plat::wall_now_ns();
    std::string path = lg::plat::package_path(dir, opts.prefix, wall);
    result.path = path;

    lg::McapWriter w;
    if (!w.open(path)) return result;
    w.begin(opts.zstd ? "zstd" : "none");

    int extra = 0;
    bool crashed =
        __atomic_load_n(&header.crashed, __ATOMIC_ACQUIRE) != 0;
    uint32_t seq = write_slots(w, recs, opts, result);
    w.write_manifest(wall, build_manifest(recs, opts, crashed ? 2 : 0, crashed,
                                          opts.redactor ? &result.redaction : nullptr));

    if (crashed) {
        std::string marker = render_crash_marker(header);
        w.write_log(seq++, 5 /*CRITICAL*/, 0, 0, lg::plat::wall_now_ns(),
                    lg::plat::mono_now_ns(), "_logguard", "", marker.c_str());
        std::string crash_stack = render_crash_stack(header);
        if (!crash_stack.empty()) {
            w.write_log(seq++, 5, 0, 0, lg::plat::wall_now_ns(),
                        lg::plat::mono_now_ns(), "_stack", "",
                        crash_stack.c_str());
        }
        extra = crash_stack.empty() ? 1 : 2;
    }

    w.finish();
    result.count = static_cast<int>(recs.size()) + extra;
    result.ok = true;
    return result;
}

} // namespace lg::export_
