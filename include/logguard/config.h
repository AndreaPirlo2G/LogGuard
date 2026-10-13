#ifndef LOGGUARD_CONFIG_H
#define LOGGUARD_CONFIG_H

/*
 * Compile-time log gates.
 *
 * Runtime level filtering (lg_set_level / SIGUSR1) still compares a level on
 * every call. Shipping builds of deeply embedded or hot-loop products often
 * want the compiler to erase entire calls — including argument construction —
 * with zero residual cost. Define one of these before including logguard.h or
 * globally (e.g. -DLG_COMPILE_MIN_LEVEL=LG_LEVEL_WARN):
 *
 *   LG_COMPILE_MIN_LEVEL   strip every call below this level (default: TRACE)
 *
 *   LG_LOGGING_ENABLED 0   compile out ALL log macros (LG_INFO, ...) while
 *                          keeping lifecycle calls (lg_init/lg_shutdown).
 *
 * These affect only the convenience macros; lg_log_to()/lg_event_to() remain
 * callable directly if you need unconditional emission.
 */

#define LG_LVL_TRACE 0
#define LG_LVL_DEBUG 1
#define LG_LVL_INFO 2
#define LG_LVL_WARN 3
#define LG_LVL_ERROR 4
#define LG_LVL_CRITICAL 5

#ifndef LG_COMPILE_MIN_LEVEL
#define LG_COMPILE_MIN_LEVEL LG_LVL_TRACE
#endif

#ifndef LG_LOGGING_ENABLED
#define LG_LOGGING_ENABLED 1
#endif

#if LG_LOGGING_ENABLED
#define LG_IF_ENABLED(level, body)                                             \
    do {                                                                       \
        if ((level) >= LG_COMPILE_MIN_LEVEL) {                                 \
            body                                                               \
        }                                                                      \
    } while (0)
#else
#define LG_IF_ENABLED(level, body) do { } while (0)
#endif

#endif /* LOGGUARD_CONFIG_H */
