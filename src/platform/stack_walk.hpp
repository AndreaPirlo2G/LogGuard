#pragma once
// Cooperative, signal-driven stack sampling.
//
// Capturing *other* threads' stacks on Linux is done by directing a
// realtime signal at each task; the kernel interrupts the target thread and it
// walks its own (valid) stack inside the handler. The platform layer owns the
// signal mechanics and tid enumeration; the recorder supplies a sink that
// persists each thread's PCs into the ring. This keeps the dangerous signal
// code in one audited place while the storage policy stays in core.
#include <cstddef>
#include <cstdint>

namespace lg::plat {

constexpr std::size_t kMaxStackFrames = 24;

// Walk the calling thread's own stack. `skip` hides inner capture frames.
// Async-signal-safe (malloc-free). Returns the number of frames written.
std::size_t capture_self(uint64_t* out, std::size_t capacity,
                         std::size_t skip = 0) noexcept;

// A signal-driven sampler. Construct once after the ring is mapped; the sink
// is invoked inside each target thread's signal context, so it must only use
// async-signal-safe primitives (the recorder passes its lock-free slot writer).
class StackSampler {
public:
    using Sink = void (*)(uint64_t tid, const uint64_t* frames,
                          std::size_t count, void* user);

    StackSampler() = default;
    ~StackSampler();
    StackSampler(const StackSampler&) = delete;
    StackSampler& operator=(const StackSampler&) = delete;

    // Register the sampling signal handler and the persistence sink.
    // Returns false if the platform does not support cooperative sampling.
    bool install(Sink sink, void* user) noexcept;

    // Direct a sampling signal at every live task. Each interrupted thread
    // invokes the installed sink from its own (valid) signal context, where the
    // recorder persists PCs into the ring — so frames never transit a shared
    // buffer. Returns the number of tasks signalled. Only safe from a normal
    // (non-signal) context; used by the watchdog while the process is alive.
    std::size_t sample_all_threads() noexcept;

private:
    bool installed_ = false;
};

} // namespace lg::plat
