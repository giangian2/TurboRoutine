#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

/*
 * Structured debug logging, compiled out entirely unless DEBUG is defined
 * (build with `make DEBUG=1`). Each line carries file:line:function so a
 * message can be traced back to its call site without a debugger attached.
 *
 * LOG_DEBUG   routine tracing: entry/exit of critical functions, sizes,
 *             counts, state transitions.
 * LOG_ERROR   a failure path was taken (allocation failure, invalid
 *             argument, I/O error) -- still just tracing, the actual error
 *             handling/return code is whatever the caller already does.
 */
#ifdef DEBUG
#define LOG_DEBUG(fmt, ...)                                                                        \
    fprintf(stderr, "[DEBUG] %s:%d %s: " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)                                                                        \
    fprintf(stderr, "[ERROR] %s:%d %s: " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#define LOG_ERROR(fmt, ...) ((void)0)
#endif

#endif /* DEBUG_H */
