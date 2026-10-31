/*
 * net.c - Camada de rede: enderecos logicos e entrega de pacotes entre nos.
 *
 * So conversa com o enlace (link.h) embaixo e com quem chamar net_send /
 * net_recv em cima. Nao sabe o que e porta, sessao ou ACK.
 */
#include "net.h"
#include "common.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    net_addr addr;
    struct sockaddr_in phys;  /* endereco fisico (enlace) */
} node_entry;

static node_entry table[NET_MAX_NODES];
static int table_len = 0;
static net_addr self_addr = 0;
static net_stats stats;

/* Resolve "host" (ip ou nome) + porta pra um sockaddr_in. */
static int resolve(const char *host, int port, struct sockaddr_in *out) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) return -1;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 0;
}

static int load_table(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    char line[256];
    table_len = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        int addr, port;
        char host[128];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%d %127s %d", &addr, host, &port) != 3 || addr < 1 || addr > 254) {
            fprintf(stderr, "net: linha invalida em %s: %s", path, line);
            continue;
        }
        if (table_len == NET_MAX_NODES) {
            fprintf(stderr, "net: tabela cheia, ignorando o resto\n");
            break;
        }
        node_entry *e = &table[table_len];
        if (resolve(host, port, &e->phys) < 0) {
            fprintf(stderr, "net: nao resolvi %s\n", host);
            continue;
        }
        e->addr = (net_addr)addr;
        table_len++;
    }
    fclose(f);
    return table_len > 0 ? 0 : -1;
}

static const node_entry *lookup(net_addr addr) {
    for (int i = 0; i < table_len; i++)
        if (table[i].addr == addr) return &table[i];
    return NULL;
}

int net_init(net_addr self, const char *nodes_file, const link_config *link_cfg) {
    if (load_table(nodes_file) < 0) return -1;
    const node_entry *me = lookup(self);
    if (me == NULL) {
        fprintf(stderr, "net: endereco %d nao esta em %s\n", self, nodes_file);
        return -1;
    }
    self_addr = self;
    memset(&stats, 0, sizeof stats);
    /* O enlace fica na porta UDP que a tabela associa a este no */
    if (link_init(ntohs(me->phys.sin_port), link_cfg) < 0) return -1;
    LOG("net", "no %d pronto, %d nos na tabela", self, table_len);
    return 0;
}

int net_send(net_addr dst, uint8_t proto, const void *data, size_t len) {
    if (len > NET_MTU) return -1;
    const node_entry *e = lookup(dst);
    if (e == NULL) {
        stats.no_route++;
        LOG("net", "sem rota para o no %d", dst);
        return -1;
    }

    /* Encapsula: cabecalho do pacote + payload da camada de cima */
    unsigned char pkt[LINK_MTU];
    pkt[0] = NET_VERSION;
    pkt[1] = proto;
    pkt[2] = self_addr;
    pkt[3] = dst;
    pkt[4] = NET_TTL;
    pkt[5] = 0;
    pkt[6] = (unsigned char)(len >> 8);
    pkt[7] = (unsigned char)(len & 0xFF);
    memcpy(pkt + NET_HDR_LEN, data, len);

    LOG("net", "TX pacote src=%d dst=%d proto=%d ttl=%d len=%zu", self_addr, dst, proto, NET_TTL, len);
    log_hex("net", "pacote", pkt, NET_HDR_LEN + len);

    stats.pkts_sent++;
    return link_send(&e->phys, pkt, NET_HDR_LEN + len);
}

int net_recv(net_addr *src, uint8_t *proto, void *buf, size_t cap, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left < 0) left = 0;

        unsigned char pkt[LINK_MTU];
        int n = link_recv(pkt, sizeof pkt, NULL, (int)left);
        if (n <= 0) return n;

        /* Desencapsula: valida o cabecalho e ve se o pacote e pra mim */
        if (n < NET_HDR_LEN || pkt[0] != NET_VERSION) {
            stats.bad_header++;
            LOG("net", "RX descartado: cabecalho invalido");
            continue;
        }
        size_t len = (size_t)(pkt[6] << 8 | pkt[7]);
        if (len != (size_t)n - NET_HDR_LEN || len > cap) {
            stats.bad_header++;
            LOG("net", "RX descartado: tamanho inconsistente");
            continue;
        }
        uint8_t ttl = pkt[4];
        if (ttl == 0) {
            stats.ttl_expired++;
            LOG("net", "RX descartado: ttl esgotado");
            continue;
        }
        pkt[4] = ttl - 1;
        if (pkt[3] != self_addr) {
            /* Sem roteamento nesta versao: pacote que nao e pra mim e descartado */
            stats.not_for_me++;
            LOG("net", "RX descartado: dst=%d nao sou eu (%d)", pkt[3], self_addr);
            continue;
        }

        log_hex("net", "pacote", pkt, (size_t)n);
        LOG("net", "RX pacote src=%d dst=%d proto=%d ttl=%d len=%zu", pkt[2], pkt[3], pkt[1], pkt[4], len);
        stats.pkts_recv++;
        if (src) *src = pkt[2];
        if (proto) *proto = pkt[1];
        memcpy(buf, pkt + NET_HDR_LEN, len);
        return (int)len;
    }
}

net_addr net_self(void) {
    return self_addr;
}

const net_stats *net_get_stats(void) {
    stats.link = *link_get_stats();
    return &stats;
}

void net_close(void) {
    link_close();
}
