#include "export/slot_render.hpp"

#include "core/constants.hpp"
#include "core/ring_flags.hpp"
#include "core/lazy_format.h"
#include "diagnosis/symbolize.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace lg::export_ {

using lg::core::Slot;
using lg::core::FileHeader;
using lg::core::PAYLOAD_CAP;

static std::string format_pcs(const char* lead, uint64_t tid,
                              const uint64_t* pc, int n) {
    char buf[PAYLOAD_CAP * 12 + 128];
    int off = std::snprintf(buf, sizeof(buf), lead,
                            static_cast<unsigned long long>(tid), n);
    for (int i = 0; i < n && off < static_cast<int>(sizeof(buf)) - 20; ++i)
        off += std::snprintf(buf + off, sizeof(buf) - off, " %lx",
                             static_cast<unsigned long>(pc[i]));
    return std::string(buf);
}

RenderedSlot render_slot(const Slot& s) { return render_slot(s, nullptr); }

RenderedSlot render_slot(const Slot& s, const lg::diag::Symbolizer* sym) {
    RenderedSlot out;
    out.level = s.level;
    out.category.assign(s.category, s.cat_len);
    out.request_id.assign(s.req_id, s.req_len);

    if (s.flags & lg::core::FLAG_STACK) {
        int nf = s.msg_len / static_cast<uint16_t>(sizeof(uint64_t));
        const uint64_t* pc = reinterpret_cast<const uint64_t*>(s.payload);
        out.category = "_stack";
        out.message = format_pcs("thread stack tid=%llu frames=%d", s.tid, pc, nf);
        if (sym && nf > 0) {
            out.message += '\n';
            out.message += sym->render_stack(pc, static_cast<std::size_t>(nf));
            if (!out.message.empty() && out.message.back() == '\n')
                out.message.pop_back();
        }
    } else if (s.flags & lg::core::FLAG_KV) {
        std::string blob(s.payload, s.msg_len);
        size_t z = blob.find('\0');
        out.message = (z == std::string::npos) ? blob : blob.substr(0, z);
        if (z != std::string::npos) out.fields_json = blob.substr(z + 1);
    } else if (s.flags & lg::core::FLAG_LAZY) {
        lg::lazy::Encoded enc{};
        std::memcpy(&enc, s.payload,
                    std::min<size_t>(s.msg_len, sizeof(enc)));
        lg::lazy::render(enc, out.message);
    } else {
        out.message.assign(s.payload, s.msg_len);
    }
    return out;
}

std::string render_crash_stack(const FileHeader& h) {
    if (h.crash_nframes <= 0) return {};
    // lead has no tid argument for the crashing header stack; use a plain form.
    char buf[PAYLOAD_CAP * 12 + 128];
    int off = std::snprintf(buf, sizeof(buf),
                            "crashing thread stack frames=%d", h.crash_nframes);
    for (int i = 0; i < h.crash_nframes &&
                    off < static_cast<int>(sizeof(buf)) - 20; ++i)
        off += std::snprintf(buf + off, sizeof(buf) - off, " %lx",
                             static_cast<unsigned long>(h.crash_frames[i]));
    return std::string(buf);
}

std::string render_crash_marker(const FileHeader& h) {
    char marker[256];
    std::snprintf(marker, sizeof(marker),
                  "logguard crash marker: signal=%d fault_addr=0x%llx mono_ns=%lld",
                  h.sig, static_cast<unsigned long long>(h.fault_addr),
                  static_cast<long long>(h.crash_mono_ns));
    return std::string(marker);
}

} // namespace lg::export_
