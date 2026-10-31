#ifndef LINK_H
#define LINK_H

#include <netinet/in.h>
#include <stddef.h>

#define LINK_MAGIC      0xAB
#define LINK_HDR_LEN    6
#define LINK_FRAME_MAX  1400                      /* cabe num datagrama sem fragmentar */
#define LINK_MTU        (LINK_FRAME_MAX - LINK_HDR_LEN)  /* maior payload aceito */

typedef struct {
    double   loss;       /* probabilidade de perda de um quadro, 0..1 */
    double   corrupt;    /* probabilidade de corromper um quadro, 0..1 */
    int      delay_ms;   /* atraso fixo de transmissao */
    int      jitter_ms;  /* atraso extra aleatorio, uniforme em [0, jitter] */
    unsigned seed;       /* semente do gerador; 0 = usa o relogio */
} link_config;

typedef struct {
    unsigned long frames_sent, frames_recv;
    unsigned long bytes_sent, bytes_recv;   /* contando o cabecalho do quadro */
    unsigned long bad_checksum;             /* quadros descartados por erro */
    unsigned long bad_format;               /* magic ou tamanho errado */
    unsigned long sim_lost, sim_corrupted, sim_delayed;  /* defeitos simulados */
} link_stats;

/* Abre o "meio fisico": um soquete UDP na porta dada. cfg pode ser NULL
 * (sem defeitos simulados). Retorna 0 ou -1. */
int link_init(int udp_port, const link_config *cfg);

/* Monta um quadro com os dados e manda pro endereco fisico `to`, passando
 * pela simulacao de defeitos. Retorna 0 ou -1 (payload maior que LINK_MTU
 * ou erro no soquete). Um quadro "perdido" pela simulacao retorna 0: quem
 * chamou nao tem como saber, exatamente como num meio fisico real. */
int link_send(const struct sockaddr_in *to, const void *data, size_t len);

/* Espera ate timeout_ms por um quadro valido. Devolve o payload em buf.
 * Retorna o tamanho do payload, 0 se nada chegou no prazo, -1 em erro.
 * Quadros com checksum errado sao descartados em silencio (so contados). */
int link_recv(void *buf, size_t cap, struct sockaddr_in *from, int timeout_ms);

const link_stats *link_get_stats(void);
void link_close(void);

#endif
