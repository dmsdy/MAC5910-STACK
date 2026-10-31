/*
 * transport.c - Camada de transporte: portas, numeracao, multiplexacao,
 * ACK e retransmissao (parada-e-espera).
 *
 * So conversa com a rede (net.h) embaixo e com a aplicacao em cima.
 */
#include "transport.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Uma mensagem guardada na fila de uma porta */
typedef struct {
    net_addr src;
    tp_port sport;
    uint16_t len;
    unsigned char data[TP_MTU];
} tp_msg;

/* Fila circular de uma porta aberta */
typedef struct {
    int open;
    tp_port port;
    int head, count;
    tp_msg q[TP_QUEUE_LEN];
} tp_queue;

/* Ultimo seq visto de cada origem (no, porta): detecta duplicatas e buracos */
typedef struct {
    int used;
    net_addr node;
    tp_port port;
    uint32_t last_seq;
} tp_peer;
#define TP_MAX_PEERS 32

/* O DATA que esta esperando ACK (parada-e-espera: no maximo um por vez) */
static struct {
    int active;
    net_addr node;
    tp_port sport;
    uint32_t seq;
    int acked;
} pending;

static tp_queue ports[TP_MAX_PORTS];
static tp_peer peers[TP_MAX_PEERS];
static int next_peer = 0;
static uint32_t next_seq;
static int timeout_ms = TP_DEFAULT_TIMEOUT_MS;
static int max_retries = TP_DEFAULT_RETRIES;
static tp_stats stats;

int tp_init(net_addr self, const char *nodes_file, const link_config *link_cfg,
            int tmo, int retries) {
    memset(ports, 0, sizeof ports);
    memset(peers, 0, sizeof peers);
    memset(&stats, 0, sizeof stats);
    memset(&pending, 0, sizeof pending);
    if (tmo > 0) timeout_ms = tmo;
    if (retries >= 0) max_retries = retries;
    /* seq inicial aleatorio, como no TCP: um no reiniciado nao confunde o
     * receptor com numeros que ele ja viu */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    next_seq = (uint32_t)rand();
    if (net_init(self, nodes_file, link_cfg) < 0) return -1;
    LOG("tp", "transporte pronto, seq inicial=%u timeout=%dms tentativas=%d",
        next_seq, timeout_ms, max_retries);
    return 0;
}

static tp_queue *find_port(tp_port port) {
    for (int i = 0; i < TP_MAX_PORTS; i++)
        if (ports[i].open && ports[i].port == port) return &ports[i];
    return NULL;
}

int tp_bind(tp_port port) {
    if (find_port(port) != NULL) return -1;
    for (int i = 0; i < TP_MAX_PORTS; i++) {
        if (!ports[i].open) {
            ports[i].open = 1;
            ports[i].port = port;
            ports[i].head = ports[i].count = 0;
            LOG("tp", "porta %u aberta", port);
            return 0;
        }
    }
    return -1;
}

static tp_peer *find_peer(net_addr node, tp_port port) {
    for (int i = 0; i < TP_MAX_PEERS; i++)
        if (peers[i].used && peers[i].node == node && peers[i].port == port) return &peers[i];
    /* nao conhece: ocupa a proxima vaga (circular) */
    tp_peer *p = &peers[next_peer];
    next_peer = (next_peer + 1) % TP_MAX_PEERS;
    p->used = 0;
    p->node = node;
    p->port = port;
    return p;
}

/* Monta o cabecalho do segmento em seg (TP_HDR_LEN bytes). */
static void build_header(unsigned char *seg, uint8_t type, tp_port sport, tp_port dport,
                         uint32_t seq, size_t len) {
    seg[0] = type;
    seg[1] = 0;
    seg[2] = (unsigned char)(sport >> 8);  seg[3] = (unsigned char)sport;
    seg[4] = (unsigned char)(dport >> 8);  seg[5] = (unsigned char)dport;
    seg[6] = (unsigned char)(seq >> 24);   seg[7] = (unsigned char)(seq >> 16);
    seg[8] = (unsigned char)(seq >> 8);    seg[9] = (unsigned char)seq;
    seg[10] = (unsigned char)(len >> 8);   seg[11] = (unsigned char)len;
}

static int send_ack(net_addr dst, tp_port sport, tp_port dport, uint32_t seq) {
    unsigned char seg[TP_HDR_LEN];
    build_header(seg, TP_TYPE_ACK, sport, dport, seq, 0);
    LOG("tp", "TX ACK %u:%u -> %d:%u seq=%u", net_self(), sport, dst, dport, seq);
    stats.acks_sent++;
    return net_send(dst, NET_PROTO_TP, seg, TP_HDR_LEN);
}

int tp_send_unreliable(net_addr dst, tp_port dport, tp_port sport, const void *data, size_t len) {
    if (len > TP_MTU) return -1;
    unsigned char seg[NET_MTU];
    uint32_t seq = next_seq++;
    build_header(seg, TP_TYPE_UDATA, sport, dport, seq, len);
    memcpy(seg + TP_HDR_LEN, data, len);

    LOG("tp", "TX UDATA %u:%u -> %d:%u seq=%u len=%zu", net_self(), sport, dst, dport, seq, len);
    log_hex("tp", "segmento", seg, TP_HDR_LEN + len);

    stats.udata_sent++;
    stats.app_bytes_sent += len;
    return net_send(dst, NET_PROTO_TP, seg, TP_HDR_LEN + len);
}

/* Guarda uma mensagem na fila da porta. Retorna 0 ou -1 (fila cheia). */
static int enqueue(tp_queue *q, net_addr src, tp_port sport, const unsigned char *data, size_t len) {
    if (q->count == TP_QUEUE_LEN) return -1;
    tp_msg *m = &q->q[(q->head + q->count) % TP_QUEUE_LEN];
    m->src = src;
    m->sport = sport;
    m->len = (uint16_t)len;
    memcpy(m->data, data, len);
    q->count++;
    return 0;
}

/* Recebe um pacote da rede (esperando ate timeout_ms) e trata o segmento
 * que ele carrega. Retorna 1 se tratou algo, 0 se nada chegou, -1 em erro. */
static int process_one(int tmo) {
    unsigned char seg[NET_MTU];
    net_addr src;
    uint8_t proto;
    int n = net_recv(&src, &proto, seg, sizeof seg, tmo);
    if (n <= 0) return n;
    if (proto != NET_PROTO_TP) return 1; /* nao e pra esta camada */

    /* Desencapsula o segmento */
    if (n < TP_HDR_LEN) {
        stats.bad_header++;
        return 1;
    }
    uint8_t type = seg[0];
    tp_port sport = (tp_port)(seg[2] << 8 | seg[3]);
    tp_port dport = (tp_port)(seg[4] << 8 | seg[5]);
    uint32_t seq = (uint32_t)seg[6] << 24 | (uint32_t)seg[7] << 16 | (uint32_t)seg[8] << 8 | seg[9];
    size_t len = (size_t)(seg[10] << 8 | seg[11]);
    if (len != (size_t)n - TP_HDR_LEN) {
        stats.bad_header++;
        LOG("tp", "RX descartado: tamanho inconsistente");
        return 1;
    }
    log_hex("tp", "segmento", seg, (size_t)n);

    /* --- ACK: confirma o DATA pendente, se for ele --- */
    if (type == TP_TYPE_ACK) {
        stats.acks_recv++;
        if (pending.active && !pending.acked && src == pending.node &&
            dport == pending.sport && seq == pending.seq) {
            pending.acked = 1;
            LOG("tp", "RX ACK seq=%u: confirmado", seq);
        } else {
            stats.stale_acks++;
            LOG("tp", "RX ACK seq=%u ignorado (nada pendente com esse numero)", seq);
        }
        return 1;
    }
    if (type != TP_TYPE_DATA && type != TP_TYPE_UDATA) {
        stats.bad_header++;
        LOG("tp", "RX descartado: tipo desconhecido %d", type);
        return 1;
    }
    LOG("tp", "RX %s %d:%u -> %u:%u seq=%u len=%zu", type == TP_TYPE_DATA ? "DATA" : "UDATA",
        src, sport, net_self(), dport, seq, len);

    tp_peer *p = find_peer(src, sport);

    /* --- DATA velho: retransmissao cujo ACK se perdeu, ou copia que chegou
     * atrasada depois de um seq maior. Como o remetente e parada-e-espera,
     * todo DATA com seq <= ultimo visto ja foi entregue: so re-confirma. --- */
    if (type == TP_TYPE_DATA && p->used && (int32_t)(seq - p->last_seq) <= 0) {
        stats.dups++;
        LOG("tp", "duplicata de seq=%u (ultimo=%u), reenviando ACK", seq, p->last_seq);
        send_ack(src, dport, sport, seq);
        return 1;
    }
    if (p->used && seq != p->last_seq + 1) {
        stats.gaps++;
        LOG("tp", "fora de sequencia: esperava %u, veio %u", p->last_seq + 1, seq);
    }

    /* --- Multiplexacao: entrega na fila da porta de destino --- */
    tp_queue *q = find_port(dport);
    if (q == NULL) {
        stats.no_port++;
        LOG("tp", "RX descartado: porta %u nao esta aberta", dport);
        return 1;
    }
    if (enqueue(q, src, sport, seg + TP_HDR_LEN, len) < 0) {
        /* Sem espaco: nao confirma, o remetente vai tentar de novo mais
         * tarde. E um controle de fluxo rudimentar de graca. */
        stats.queue_full++;
        LOG("tp", "RX descartado: fila da porta %u cheia", dport);
        return 1;
    }
    p->used = 1;
    p->last_seq = seq;
    stats.app_bytes_recv += len;
    if (type == TP_TYPE_DATA) {
        stats.data_recv++;
        send_ack(src, dport, sport, seq);
    } else {
        stats.udata_recv++;
    }
    return 1;
}

int tp_send(net_addr dst, tp_port dport, tp_port sport, const void *data, size_t len) {
    if (len > TP_MTU || pending.active) return -1;
    unsigned char seg[NET_MTU];
    uint32_t seq = next_seq++;
    build_header(seg, TP_TYPE_DATA, sport, dport, seq, len);
    memcpy(seg + TP_HDR_LEN, data, len);
    stats.data_sent++;
    stats.app_bytes_sent += len;

    pending.active = 1;
    pending.node = dst;
    pending.sport = sport;
    pending.seq = seq;
    pending.acked = 0;

    int attempt;
    for (attempt = 1; attempt <= max_retries + 1; attempt++) {
        if (attempt > 1) {
            stats.retrans++;
            LOG("tp", "timeout, retransmitindo seq=%u (tentativa %d de %d)", seq, attempt, max_retries + 1);
        }
        LOG("tp", "TX DATA %u:%u -> %d:%u seq=%u len=%zu", net_self(), sport, dst, dport, seq, len);
        log_hex("tp", "segmento", seg, TP_HDR_LEN + len);
        if (net_send(dst, NET_PROTO_TP, seg, TP_HDR_LEN + len) < 0) {
            pending.active = 0;
            return -1;
        }
        /* Espera o ACK; o que chegar pras portas abertas enquanto isso e enfileirado */
        long deadline = now_ms() + timeout_ms;
        while (!pending.acked) {
            long left = deadline - now_ms();
            if (left <= 0) break;
            if (process_one((int)left) < 0) {
                pending.active = 0;
                return -1;
            }
        }
        if (pending.acked) {
            pending.active = 0;
            return attempt;
        }
    }
    pending.active = 0;
    stats.failed++;
    LOG("tp", "desisti de seq=%u depois de %d tentativas", seq, max_retries + 1);
    return -1;
}

int tp_poll(int tmo) {
    /* espera ate tmo pelo primeiro pacote; depois so esvazia o que ja
     * chegou, sem esperar mais, pra nao segurar a entrega a aplicacao */
    int r = process_one(tmo);
    while (r > 0) r = process_one(0);
    return r < 0 ? -1 : 0;
}

int tp_recv(tp_port port, net_addr *src, tp_port *sport, void *buf, size_t cap, int tmo) {
    tp_queue *q = find_port(port);
    if (q == NULL) return -1;
    long deadline = now_ms() + tmo;
    for (;;) {
        if (q->count > 0) {
            tp_msg *m = &q->q[q->head];
            size_t len = m->len < cap ? m->len : cap;
            memcpy(buf, m->data, len);
            if (src) *src = m->src;
            if (sport) *sport = m->sport;
            q->head = (q->head + 1) % TP_QUEUE_LEN;
            q->count--;
            return (int)len;
        }
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        if (process_one((int)left) < 0) return -1;
    }
}

const tp_stats *tp_get_stats(void) {
    stats.net = *net_get_stats();
    return &stats;
}

void tp_close(void) {
    net_close();
}
