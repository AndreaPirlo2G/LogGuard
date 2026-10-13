// Minimal LogGuard usage: logs for 20 iterations, then shuts down cleanly.
// Run it twice: no crash report is produced, recovery says "nothing to recover".
#include <logguard/logguard.h>

#include <unistd.h>

int main(int argc, char** argv) {
    const char* ring = argc > 1 ? argv[1] : "basic.ring";

    if (lg_init(ring) != 0) return 1;

    if (argc > 2 && std::string(argv[2]) == "lazy") {
        LG_INFO_LAZY("bench", "counter=%d ratio=%lu ok=%d", 42, (unsigned long)7, 1);
        lg_trigger_snapshot("lazy demo snapshot");
        lg_shutdown();
        return 0;
    }

    for (int i = 0; i < 20; ++i) {
        LG_INFO("basic", "iteration=%d phase=%s", i, (i % 4 == 0) ? "flush" : "steady");
        usleep(1000);
    }

    lg_shutdown();
    return 0;
}
