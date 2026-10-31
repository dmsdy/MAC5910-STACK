#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "net.h"
#include <stddef.h>
#include <stdint.h>

typedef uint16_t tp_port;

#define TP_HDR_LEN   12
#define TP_MTU       (NET_MTU - TP_HDR_LEN)   /* maior payload de um segmento */
#define TP_MAX_PORTS 8                        /* portas abertas ao mesmo tempo */
#define TP_QUEUE_LEN 32                       /* mensagens guardadas por porta */

#define TP_TYPE_DATA  1
#define TP_TYPE_ACK   2
#define TP_TYPE_UDATA 3

#define TP_DEFAULT_TIMEOUT_MS 200
#define TP_DEFAULT_RETRIES    5

typedef struct {
    unsigned long data_sent, data_recv;      /* DATA distintos (sem contar retransmissoes) */
    unsigned long udata_sent, udata_recv;
    unsigned long acks_sent, acks_recv;
    unsigned long retrans;      /* retransmissoes de DATA */
    unsigned long failed;       /* DATA que desistiu depois de max_retries */
    unsigned long dups;         /* DATA repetidos descartados no receptor */
    unsigned long stale_acks;   /* ACK que nao casava com o DATA pendente */
    unsigned long gaps;         /* segmentos que chegaram fora da sequencia esperada */
    unsigned long no_port;      /* chegou pra uma porta que ninguem abriu */
    unsigned long queue_full;   /* descartados porque a fila da porta encheu */
    unsigned long bad_header;
    unsigned long app_bytes_sent, app_bytes_recv;  /* so payload, pra medir overhead */
    net_stats net;              /* copia das estatisticas da camada de baixo */
} tp_stats;

/* Sobe a pilha deste no (rede + enlace). timeout_ms e max_retries
 * controlam a retransmissao; 0 usa o padrao. Retorna 0 ou -1. */
int tp_init(net_addr self, const char *nodes_file, const link_config *link_cfg,
            int timeout_ms, int max_retries);

/* Abre uma porta pra receber. Retorna 0, ou -1 se ja aberta / sem espaco. */
int tp_bind(tp_port port);

/* Manda `len` bytes pra (dst, dport) com confirmacao. Bloqueia ate o ACK
 * chegar ou as tentativas acabarem. Retorna quantas transmissoes foram
 * precisas (1 = de primeira), ou -1 se desistiu. Enquanto espera, continua
 * recebendo e enfileirando o que chega pras portas abertas. */
int tp_send(net_addr dst, tp_port dport, tp_port sport, const void *data, size_t len);

/* Manda `len` bytes pra (dst, dport), sem confirmacao. Retorna 0 ou -1. */
int tp_send_unreliable(net_addr dst, tp_port dport, tp_port sport, const void *data, size_t len);

/* Espera ate timeout_ms por uma mensagem na porta `port`. Preenche src e
 * sport com quem mandou. Retorna o tamanho, 0 se nada chegou, -1 em erro.
 * Mensagens pra outras portas que chegarem enquanto isso ficam guardadas
 * nas filas delas (multiplexacao). */
int tp_recv(tp_port port, net_addr *src, tp_port *sport, void *buf, size_t cap, int timeout_ms);

/* Processa o que chegou da rede por ate timeout_ms, enfileirando nas
 * portas, sem esperar por uma porta especifica. Retorna 0 ou -1. */
int tp_poll(int timeout_ms);

const tp_stats *tp_get_stats(void);
void tp_close(void);

#endif
