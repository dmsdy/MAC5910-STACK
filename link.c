/*
 * link.c - Camada de enlace: quadros com checksum sobre UDP, com simulacao
 * de perda, corrupcao e atraso.
 */
#include "link.h"
#include "common.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int sock = -1;
static link_config cfg;
static link_stats stats;
static unsigned rng_state;

/* Fila de quadros atrasados: cada um tem a hora em que deve ir pro soquete. */
#define DELAY_QUEUE_MAX 256
typedef struct {
    long due;
    unsigned long order;      /* ordem de chegada, desempata vencimentos iguais */
    struct sockaddr_in to;
    size_t len;
    unsigned char bytes[LINK_FRAME_MAX];
} delayed_frame;
static delayed_frame dq[DELAY_QUEUE_MAX];
static int dq_count = 0;
static unsigned long dq_order = 0;

/* Gerador proprio (rand_r) pra que os experimentos sejam reproduziveis
 * com a mesma semente, independente do que o resto do programa faz. */
static double rnd01(void) {
    return rand_r(&rng_state) / (RAND_MAX + 1.0);
}

/* Checksum da Internet (RFC 1071): soma em complemento de um das palavras
 * de 16 bits, com o "vai-um" dobrado de volta, e complemento no final.
 * Propriedade util: a soma de um buffer que ja contem seu checksum da 0xFFFF. */
static uint16_t checksum(const unsigned char *data, size_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)(data[0] << 8 | data[1]);
        data += 2;
        len -= 2;
    }
    if (len == 1) sum += (uint32_t)(data[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

int link_init(int udp_port, const link_config *c) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("link: socket");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(udp_port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("link: bind");
        close(sock);
        sock = -1;
        return -1;
    }
    memset(&stats, 0, sizeof stats);
    memset(&cfg, 0, sizeof cfg);
    if (c) cfg = *c;
    rng_state = cfg.seed ? cfg.seed : (unsigned)time(NULL) ^ (unsigned)getpid();
    dq_count = 0;
    LOG("link", "meio fisico pronto: UDP porta %d (perda=%.2f corrupcao=%.2f atraso=%d+%dms)",
        udp_port, cfg.loss, cfg.corrupt, cfg.delay_ms, cfg.jitter_ms);
    return 0;
}

/* Entrega de fato um quadro ao soquete. */
static int wire_send(const struct sockaddr_in *to, const unsigned char *frame, size_t flen) {
    if (sendto(sock, frame, flen, 0, (const struct sockaddr *)to, sizeof *to) < 0) {
        perror("link: sendto");
        return -1;
    }
    stats.frames_sent++;
    stats.bytes_sent += flen;
    return 0;
}

/* Manda pro soquete todos os quadros atrasados cuja hora ja chegou.
 * Retorna quantos ms faltam pro proximo, ou -1 se a fila esta vazia. */
static long flush_delayed(void) {
    long now = now_ms();
    /* Sempre despacha primeiro o quadro que venceu ha mais tempo (e, em
     * empate, o que entrou antes): sem jitter o enlace preserva a ordem,
     * e so o jitter reordena, como num meio fisico de verdade. */
    for (;;) {
        int best = -1;
        for (int i = 0; i < dq_count; i++) {
            if (dq[i].due > now) continue;
            if (best < 0 || dq[i].due < dq[best].due ||
                (dq[i].due == dq[best].due && dq[i].order < dq[best].order))
                best = i;
        }
        if (best < 0) break;
        LOG("link", "atraso vencido, quadro de %zu bytes vai pro fio", dq[best].len);
        wire_send(&dq[best].to, dq[best].bytes, dq[best].len);
        dq[best] = dq[dq_count - 1];
        dq_count--;
    }
    long next = -1;
    for (int i = 0; i < dq_count; i++) {
        long wait = dq[i].due - now;
        if (next < 0 || wait < next) next = wait;
    }
    return next;
}

int link_send(const struct sockaddr_in *to, const void *data, size_t len) {
    if (sock < 0 || len > LINK_MTU) return -1;

    unsigned char frame[LINK_FRAME_MAX];
    size_t flen = LINK_HDR_LEN + len;

    /* Encapsula: cabecalho + payload da camada de cima */
    frame[0] = LINK_MAGIC;
    frame[1] = 0;
    frame[2] = (unsigned char)(len >> 8);
    frame[3] = (unsigned char)(len & 0xFF);
    frame[4] = 0;
    frame[5] = 0;
    memcpy(frame + LINK_HDR_LEN, data, len);
    uint16_t c = checksum(frame, flen);
    frame[4] = (unsigned char)(c >> 8);
    frame[5] = (unsigned char)(c & 0xFF);

    LOG("link", "TX quadro len=%zu checksum=0x%04x -> %s:%d", len, c,
        inet_ntoa(to->sin_addr), ntohs(to->sin_port));
    log_hex("link", "quadro", frame, flen);

    flush_delayed();

    /* --- simulacao do meio fisico --- */
    if (cfg.loss > 0 && rnd01() < cfg.loss) {
        stats.sim_lost++;
        LOG("link", "SIM: quadro perdido no meio fisico");
        return 0;
    }
    if (cfg.corrupt > 0 && rnd01() < cfg.corrupt) {
        size_t bit = (size_t)(rnd01() * flen * 8);
        frame[bit / 8] ^= (unsigned char)(1u << (bit % 8));
        stats.sim_corrupted++;
        LOG("link", "SIM: bit %zu invertido (byte %zu)", bit, bit / 8);
    }
    if (cfg.delay_ms > 0 || cfg.jitter_ms > 0) {
        long d = cfg.delay_ms;
        if (cfg.jitter_ms > 0) d += (long)(rnd01() * (cfg.jitter_ms + 1));
        if (dq_count == DELAY_QUEUE_MAX) {
            /* fila cheia: o mais antigo sai agora pra abrir espaco */
            int oldest = 0;
            for (int i = 1; i < dq_count; i++)
                if (dq[i].due < dq[oldest].due) oldest = i;
            wire_send(&dq[oldest].to, dq[oldest].bytes, dq[oldest].len);
            dq[oldest] = dq[dq_count - 1];
            dq_count--;
        }
        delayed_frame *f = &dq[dq_count++];
        f->due = now_ms() + d;
        f->order = dq_order++;
        f->to = *to;
        f->len = flen;
        memcpy(f->bytes, frame, flen);
        stats.sim_delayed++;
        LOG("link", "SIM: quadro segurado por %ldms", d);
        return 0;
    }

    return wire_send(to, frame, flen);
}

int link_recv(void *buf, size_t cap, struct sockaddr_in *from, int timeout_ms) {
    if (sock < 0) return -1;
    long deadline = now_ms() + timeout_ms;

    /* Fica no laco ate chegar um quadro bom ou estourar o prazo: um quadro
     * corrompido nao deve "gastar" a espera inteira de quem chamou. */
    for (;;) {
        long next = flush_delayed();
        long left = deadline - now_ms();
        if (left < 0) left = 0;
        /* nao dorme alem da hora do proximo quadro atrasado */
        long wait = (next >= 0 && next < left) ? next : left;

        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        int r = poll(&pfd, 1, (int)wait);
        if (r < 0) {
            perror("link: poll");
            return -1;
        }
        if (r == 0) {
            if (now_ms() >= deadline) return 0; /* prazo esgotado */
            continue;                            /* so acordou pra despachar atrasados */
        }

        unsigned char frame[LINK_FRAME_MAX];
        struct sockaddr_in src;
        socklen_t slen = sizeof src;
        ssize_t n = recvfrom(sock, frame, sizeof frame, 0, (struct sockaddr *)&src, &slen);
        if (n < 0) {
            perror("link: recvfrom");
            return -1;
        }
        stats.frames_recv++;
        stats.bytes_recv += (unsigned long)n;

        /* Desencapsula: confere magic, tamanho e checksum */
        if (n < LINK_HDR_LEN || frame[0] != LINK_MAGIC) {
            stats.bad_format++;
            LOG("link", "RX descartado: nao e um quadro valido (%zd bytes)", n);
            continue;
        }
        size_t len = (size_t)(frame[2] << 8 | frame[3]);
        if (len != (size_t)n - LINK_HDR_LEN || len > cap) {
            stats.bad_format++;
            LOG("link", "RX descartado: tamanho inconsistente (len=%zu, recebido=%zd)", len, n);
            continue;
        }
        if (checksum(frame, (size_t)n) != 0) {
            stats.bad_checksum++;
            LOG("link", "RX descartado: checksum invalido (len=%zu)", len);
            continue;
        }

        log_hex("link", "quadro", frame, (size_t)n);
        LOG("link", "RX quadro len=%zu checksum ok <- %s:%d", len,
            inet_ntoa(src.sin_addr), ntohs(src.sin_port));
        memcpy(buf, frame + LINK_HDR_LEN, len);
        if (from) *from = src;
        return (int)len;
    }
}

const link_stats *link_get_stats(void) {
    return &stats;
}

void link_close(void) {
    /* despacha o que ainda esta na fila de atraso, esperando se preciso */
    while (dq_count > 0) {
        long next = flush_delayed();
        if (next > 0) poll(NULL, 0, (int)next);
    }
    if (sock >= 0) close(sock);
    sock = -1;
}
