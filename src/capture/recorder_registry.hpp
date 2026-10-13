#pragma once
// Internal instance registry for the capture layer.
//
// LogGuard supports several independent Recorder instances in one process. A
// small fixed-size slot table maps cheap integer handles to live Recorders so
// the C ABI never hands out raw C++ pointers. Exactly one registered instance
// is the *primary*: it owns the process-global crash handler, the cooperative
// stack-sampling signal and SIGUSR1 runtime-level toggling, because signal
// dispositions are per-process. Additional instances are ordinary ring writers
// with their own level, rules and export worker.
//
// The ambient (thread-local) request/correlation id and the "current recorder"
// for the legacy zero-handle macros live here too.
#include <cstddef>
#include <cstdint>

namespace lg::cap {

class Recorder;

inline constexpr int kMaxRecorders = 8;
inline constexpr int kInvalidHandle = -1;
inline constexpr int kRequestCap = 24; // must match core Slot::req_id width

// Register/unregister. The first registered instance becomes primary.
int registry_add(Recorder* r);
void registry_remove(int handle);

Recorder* registry_get(int handle);
int registry_primary_handle();
Recorder* registry_primary();

// Thread-local "current" recorder (default = primary handle). Used by the
// legacy LG_INFO style macros which carry no explicit handle.
void registry_set_current(int handle);
int registry_current_handle();
Recorder* registry_current();

// Thread-local correlation/request id shared by whichever recorder is current.
void registry_set_request(const char* id);
const char* registry_request();
void registry_clear_request();

} // namespace lg::cap
