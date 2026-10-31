#ifndef NET_H
#define NET_H

#include "link.h"
#include <stddef.h>
#include <stdint.h>

typedef uint8_t net_addr;

#define NET_VERSION   1
#define NET_HDR_LEN   8
#define NET_MTU       (LINK_MTU - NET_HDR_LEN)   /* maior payload de um pacote */
#define NET_MAX_NODES 32
#define NET_TTL       16

/* Protocolos da camada de cima */
#define NET_PROTO_RAW 0   /* teste: payload cru */
#define NET_PROTO_TP  1   /* transporte da mini-pilha */

typedef struct {
    unsigned long pkts_sent, pkts_recv;
    unsigned long no_route;      /* destino nao esta na tabela */
    unsigned long not_for_me;    /* dst != meu endereco */
    unsigned long bad_header;    /* versao/tamanho invalidos */
    unsigned long ttl_expired;
    link_stats link;             /* copia das estatisticas da camada de baixo */
} net_stats;

/* Le a tabela de nos, descobre o proprio endereco fisico nela e sobe o
 * enlace. link_cfg pode ser NULL. Retorna 0 ou -1. */
int net_init(net_addr self, const char *nodes_file, const link_config *link_cfg);

/* Encapsula os dados num pacote e manda pro no `dst`. Retorna 0 ou -1
 * (sem rota, payload maior que NET_MTU ou erro no enlace). */
int net_send(net_addr dst, uint8_t proto, const void *data, size_t len);

/* Espera ate timeout_ms por um pacote enderecado a este no. Devolve o
 * payload em buf e preenche src e proto. Retorna o tamanho, 0 se nada
 * chegou, -1 em erro. */
int net_recv(net_addr *src, uint8_t *proto, void *buf, size_t cap, int timeout_ms);

net_addr net_self(void);
const net_stats *net_get_stats(void);
void net_close(void);

#endif
