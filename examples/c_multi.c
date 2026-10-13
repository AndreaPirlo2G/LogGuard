/* Pure-C demo of the 0.4 multi-instance ABI: two independent black boxes in one
 * process. The first opened handle is primary (owns crash/signals); the second
 * is an isolated ring writer. Build via CMake target c_multi.
 *   ./c_multi <ring_a> <ring_b>
 */
#include <logguard/c_api.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* ra = argc > 1 ? argv[1] : "multi_a.ring";
    const char* rb = argc > 2 ? argv[2] : "multi_b.ring";

    lg_opts2_t oa = {0, 0, 0, 0, 0};
    int a = lg_open(ra, &oa);
    int b = lg_open(rb, &oa);
    if (a < 0 || b < 0) {
        fprintf(stderr, "open failed a=%d b=%d\n", a, b);
        return 1;
    }
    printf("handles: a=%d b=%d version=%s\n", a, b, lg_version());

    for (int i = 0; i < 6; ++i) {
        lg_log_to(a, 2, "alpha", "alpha event %d", i);
        lg_log_to(b, 2, "beta", "beta event %d", i);
    }
    lg_set_level_to(b, 3);
    printf("level b=%d\n", lg_get_level_to(b));

    lg_health_t ha, hb;
    lg_health_to(a, &ha);
    lg_health_to(b, &hb);
    printf("health a trunc=%llu b trunc=%llu\n",
           (unsigned long long)ha.truncated_msgs,
           (unsigned long long)hb.truncated_msgs);

    lg_close(a);
    lg_close(b);
    return 0;
}
