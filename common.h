#ifndef COMMON_H
#define COMMON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

extern int g_verbose;

#define LOG(layer, ...)                                   \
    do {                                                  \
        if (g_verbose) {                                  \
            fprintf(stderr, "  [%-5s] ", layer);          \
            fprintf(stderr, __VA_ARGS__);                 \
            fputc('\n', stderr);                          \
        }                                                 \
    } while (0)

/* Mostra os primeiros bytes de um buffer em hexa (pra ver os cabecalhos). */
static inline void log_hex(const char *layer, const char *label,
                           const void *data, size_t len) {
    if (!g_verbose) return;
    const unsigned char *p = data;
    size_t show = len < 24 ? len : 24;
    fprintf(stderr, "  [%-5s] %s (%zu bytes):", layer, label, len);
    for (size_t i = 0; i < show; i++) fprintf(stderr, " %02x", p[i]);
    if (show < len) fprintf(stderr, " ...");
    fputc('\n', stderr);
}

/* Relogio monotonico em milissegundos. */
static inline long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#endif
