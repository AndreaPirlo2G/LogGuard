/* Pure-C consumer of the stable LogGuard ABI. Build:
 *   cc -std=c11 examples/c_abi.c -Iinclude -L build -llogguard -lpthread -o c_abi
 * Proves the library is usable from C (and by extension P/Invoke / cffi). */
#include <logguard/c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* ring = argc > 1 ? argv[1] : "c_abi.ring";
    lg_opts_t opts = {0, 1}; /* watchdog off, zstd on if available */
    if (lg_init_opts(ring, &opts) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    printf("logguard %s initialized\n", lg_version());

    /* A rule: 2 ERRORs within 1s triggers a snapshot. */
    lg_add_rule(4 /*ERROR*/, "payment", "failed", 2, 1000, 2000);

    lg_set_request("req-c-0001");
    for (int i = 0; i < 3; ++i) {
        lg_log(4, "payment", "charge failed for order %d, code=%d", 5000 + i, 53);
        usleep(100 * 1000);
    }
    lg_clear_request();

    lg_health_t h;
    lg_health(&h);
    printf("health: rule_fires=%llu snapshots=%llu\n",
           (unsigned long long)h.rule_fires,
           (unsigned long long)h.snapshots);

    lg_shutdown();
    return 0;
}
