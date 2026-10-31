/*
 * node.c - Camada de aplicacao da mini-pilha: um no da rede.
 *
 * Cada processo node e um no completo (aplicacao + transporte + rede +
 * enlace) identificado por um endereco logico. A aplicacao e um CHAT entre
 * nos com controle de sessao, mais dois servicos auxiliares usados nos
 * experimentos: um ECO e um cliente de PING. Sao tres aplicacoes em
 * portas diferentes do transporte, o que exercita a multiplexacao:
 *
 * porta 1  chat         HELLO / WELCOME / MSG / BYE / ERR
 * porta 2  cliente ping recebe os PONG
 * porta 7  servidor eco responde PING com PONG
 *
 * Protocolo de aplicacao (texto, uma mensagem por segmento):
 * HELLO <nome>           pede pra abrir sessao; o outro lado responde WELCOME
 * WELCOME <nome>         sessao aberta dos dois lados
 * MSG <texto>            mensagem de chat; so e aceita com sessao aberta
 * BYE                    encerra a sessao
 * ERR <motivo>           ex.: ERR NO_SESSION
 * PING <R|U> <id> [enchimento]   R = pede resposta confiavel, U = nao confiavel
 * PONG <R|U> <id> [enchimento]   resposta do eco, mesmo conteudo
 *
 * Uso:
 * ./node -a <endereco> [-n nome] [-f nodes.txt] [-e] [-v]
 *        [-p perda] [-c corrupcao] [-d atraso] [-j jitter] [-s seed]
 *        [-t timeout_ms] [-r tentativas] [-w espera_pong_ms]
 *        [-b <destino> -N <n> [-L <tamanho>] [-u]]
 *
 * -e  modo servidor: nao le o teclado, so responde (eco e chat)
 * -b  modo batch: faz n pings pro destino, imprime uma linha CSV e sai
 *     (-u usa o servico nao confiavel); e o que o bench usa
 *
 * Comandos no chat:
 * /connect <no>          /msg <no> <texto>        <texto>  (manda pra todas as sessoes)
 * /bye <no>              /ping <no> [n] [tam]     /uping <no> [n] [tam]
 * /sessions              /stats                   /quit
 */
#include "transport.h"
#include "common.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_verbose = 0;

#define CHAT_PORT 1
#define PING_PORT 2
#define ECHO_PORT 7
#define MAX_SESSIONS 16
#define NAME_MAX 32

/* ---- sessoes de chat ---- */
enum { S_FREE = 0, S_PENDING, S_OPEN };
typedef struct {
    int state;
    net_addr peer;
    char name[NAME_MAX];
} session;

static session sessions[MAX_SESSIONS];
static char my_name[NAME_MAX] = "anonimo";
static net_addr my_addr;
static int pong_wait_ms = 1000;

static session *find_session(net_addr peer) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].state != S_FREE && sessions[i].peer == peer) return &sessions[i];
    return NULL;
}

static session *new_session(net_addr peer) {
    session *s = find_session(peer);
    if (s) return s;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state == S_FREE) {
            sessions[i].peer = peer;
            sessions[i].name[0] = '\0';
            return &sessions[i];
        }
    }
    return NULL;
}

/* Manda uma mensagem de chat (confiavel) e avisa se nao deu. */
static int chat_send(net_addr dst, const char *msg) {
    LOG("app", "TX chat -> no %d: %s", dst, msg);
    int r = tp_send(dst, CHAT_PORT, CHAT_PORT, msg, strlen(msg));
    if (r < 0) printf("*** nao consegui entregar para o no %d (sem resposta)\n", dst);
    return r;
}

/* Trata uma mensagem que chegou na porta do chat. */
static void handle_chat(net_addr src, char *msg) {
    LOG("app", "RX chat <- no %d: %s", src, msg);
    char reply[64];
    session *s = find_session(src);

    if (strncmp(msg, "HELLO ", 6) == 0) {
        s = new_session(src);
        if (s == NULL) {
            chat_send(src, "ERR TOO_MANY_SESSIONS");
            return;
        }
        snprintf(s->name, NAME_MAX, "%.*s", NAME_MAX - 1, msg + 6);
        s->state = S_OPEN;
        printf("*** sessao aberta com %s (no %d)\n", s->name, src);
        snprintf(reply, sizeof reply, "WELCOME %s", my_name);
        chat_send(src, reply);
    } else if (strncmp(msg, "WELCOME ", 8) == 0) {
        if (s == NULL || s->state != S_PENDING) return; /* nao pedi nada */
        snprintf(s->name, NAME_MAX, "%.*s", NAME_MAX - 1, msg + 8);
        s->state = S_OPEN;
        printf("*** sessao aberta com %s (no %d)\n", s->name, src);
    } else if (strncmp(msg, "MSG ", 4) == 0) {
        if (s == NULL || s->state != S_OPEN) {
            chat_send(src, "ERR NO_SESSION");
            return;
        }
        printf("<%s@%d> %s\n", s->name, src, msg + 4);
    } else if (strcmp(msg, "BYE") == 0) {
        if (s != NULL) {
            printf("*** %s (no %d) encerrou a sessao\n", s->name, src);
            s->state = S_FREE;
        }
    } else if (strncmp(msg, "ERR ", 4) == 0) {
        printf("*** erro do no %d: %s\n", src, msg + 4);
        if (strcmp(msg + 4, "NO_SESSION") == 0 && s) s->state = S_FREE;
    } else {
        printf("*** mensagem desconhecida do no %d: %s\n", src, msg);
    }
    fflush(stdout);
}

/* Servidor de eco: devolve PONG com o mesmo conteudo, pelo mesmo servico. */
static void handle_echo(net_addr src, tp_port sport, char *msg, size_t len) {
    if (len < 7 || strncmp(msg, "PING ", 5) != 0) return;
    LOG("app", "RX eco <- no %d: %.20s", src, msg);
    msg[1] = 'O';
    msg[2] = 'N';   /* PING -> PONG, resto igual */
    if (msg[5] == 'R')
        tp_send(src, sport, ECHO_PORT, msg, len);
    else
        tp_send_unreliable(src, sport, ECHO_PORT, msg, len);
}

/* Processa tudo que chegou pras tres aplicacoes, esperando ate wait_ms. */
static void service(int wait_ms) {
    static char buf[TP_MTU + 1];
    net_addr src;
    tp_port sport;
    int n;
    tp_poll(wait_ms);
    while ((n = tp_recv(CHAT_PORT, &src, &sport, buf, TP_MTU, 0)) > 0) {
        buf[n] = '\0';
        handle_chat(src, buf);
    }
    while ((n = tp_recv(ECHO_PORT, &src, &sport, buf, TP_MTU, 0)) > 0) {
        buf[n] = '\0';
        handle_echo(src, sport, buf, (size_t)n);
    }
    /* PONG atrasado que ninguem espera mais: descarta */
    while (tp_recv(PING_PORT, &src, &sport, buf, TP_MTU, 0) > 0) {}
}

/* ---- ping ---- */
typedef struct {
    int sent, delivered, failed;
    long rtt_sum, rtt_max, total_ms;
    unsigned long retrans;
} ping_result;

static void do_ping(net_addr dst, int n, int size, int reliable, int quiet, ping_result *r) {
    static char msg[TP_MTU + 1], buf[TP_MTU + 1];
    memset(r, 0, sizeof *r);
    unsigned long retrans0 = tp_get_stats()->retrans;
    long t_start = now_ms();

    for (int i = 1; i <= n; i++) {
        int len = snprintf(msg, sizeof msg, "PING %c %d ", reliable ? 'R' : 'U', i);
        if (size > len) {
            memset(msg + len, 'x', (size_t)(size - len));
            len = size;
        }
        long t0 = now_ms();
        r->sent++;
        if (reliable) {
            if (tp_send(dst, ECHO_PORT, PING_PORT, msg, (size_t)len) < 0) {
                r->failed++;
                if (!quiet) printf("ping %d: sem resposta (desisti)\n", i);
                continue;
            }
        } else {
            tp_send_unreliable(dst, ECHO_PORT, PING_PORT, msg, (size_t)len);
        }
        /* espera o PONG deste ping; o resto que chegar fica enfileirado */
        long deadline = now_ms() + pong_wait_ms;
        int got = 0;
        for (;;) {
            long left = deadline - now_ms();
            if (left <= 0) break;
            net_addr src;
            tp_port sport;
            int m = tp_recv(PING_PORT, &src, &sport, buf, TP_MTU, (int)left);
            if (m <= 0) break;
            buf[m] = '\0';
            int id;
            if (sscanf(buf, "PONG %*c %d", &id) == 1 && id == i && src == dst) {
                got = 1;
                break;
            }
        }
        long rtt = now_ms() - t0;
        if (got) {
            r->delivered++;
            r->rtt_sum += rtt;
            if (rtt > r->rtt_max) r->rtt_max = rtt;
            if (!quiet) printf("ping %d: %d bytes do no %d, rtt=%ldms\n", i, len, dst, rtt);
        } else {
            r->failed++;
            if (!quiet) printf("ping %d: sem resposta\n", i);
        }
        fflush(stdout);
    }
    r->total_ms = now_ms() - t_start;
    r->retrans = tp_get_stats()->retrans - retrans0;
}

static void print_ping_summary(const ping_result *r) {
    printf("--- %d enviados, %d respondidos, %d sem resposta, %lu retransmissoes, rtt medio %.1fms, max %ldms, total %ldms\n",
           r->sent, r->delivered, r->failed, r->retrans,
           r->delivered ? (double)r->rtt_sum / r->delivered : 0.0, r->rtt_max, r->total_ms);
}

static void print_stats(void) {
    const tp_stats *s = tp_get_stats();
    const net_stats *n = &s->net;
    const link_stats *l = &n->link;
    printf("transporte: DATA tx=%lu rx=%lu | UDATA tx=%lu rx=%lu | ACK tx=%lu rx=%lu | retrans=%lu falhas=%lu dup=%lu fora_seq=%lu sem_porta=%lu\n",
           s->data_sent, s->data_recv, s->udata_sent, s->udata_recv, s->acks_sent, s->acks_recv,
           s->retrans, s->failed, s->dups, s->gaps, s->no_port);
    printf("rede:       pacotes tx=%lu rx=%lu | sem_rota=%lu nao_pra_mim=%lu cabecalho_ruim=%lu\n",
           n->pkts_sent, n->pkts_recv, n->no_route, n->not_for_me, n->bad_header);
    printf("enlace:     quadros tx=%lu rx=%lu | checksum_ruim=%lu formato_ruim=%lu | sim: perdidos=%lu corrompidos=%lu atrasados=%lu\n",
           l->frames_sent, l->frames_recv, l->bad_checksum, l->bad_format, l->sim_lost, l->sim_corrupted, l->sim_delayed);
    unsigned long app = s->app_bytes_sent + s->app_bytes_recv;
    unsigned long wire = l->bytes_sent + l->bytes_recv;
    printf("bytes:      aplicacao=%lu fio=%lu overhead=%.2fx\n", app, wire, app ? (double)wire / app : 0.0);
}

/* ---- comandos do usuario ---- */
static void command(char *line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (line[0] == '\0') return;

    if (line[0] != '/') {
        /* texto puro: manda pra todas as sessoes abertas */
        char msg[TP_MTU];
        snprintf(msg, sizeof msg, "MSG %s", line);
        int sent = 0;
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (sessions[i].state == S_OPEN) {
                chat_send(sessions[i].peer, msg);
                sent++;
            }
        if (!sent) printf("*** nenhuma sessao aberta; use /connect <no>\n");
        return;
    }

    char cmd[32];
    int a = 0, b = 0, c = 0;
    int nargs = sscanf(line, "%31s %d %d %d", cmd, &a, &b, &c) - 1;

    if (strcmp(cmd, "/connect") == 0 && nargs >= 1) {
        session *s = new_session((net_addr)a);
        if (s == NULL) { printf("*** sem espaco pra mais sessoes\n"); return; }
        if (s->state == S_OPEN) { printf("*** ja existe sessao com o no %d\n", a); return; }
        s->state = S_PENDING;
        char msg[64];
        snprintf(msg, sizeof msg, "HELLO %s", my_name);
        if (chat_send((net_addr)a, msg) < 0) s->state = S_FREE;
    } else if (strcmp(cmd, "/msg") == 0 && nargs >= 1) {
        session *s = find_session((net_addr)a);
        if (s == NULL || s->state != S_OPEN) { printf("*** sem sessao com o no %d\n", a); return; }
        char *text = strchr(line + 5, ' ');
        if (text == NULL) return;
        char msg[TP_MTU];
        snprintf(msg, sizeof msg, "MSG %s", text + 1);
        chat_send((net_addr)a, msg);
    } else if (strcmp(cmd, "/bye") == 0 && nargs >= 1) {
        session *s = find_session((net_addr)a);
        if (s == NULL) { printf("*** sem sessao com o no %d\n", a); return; }
        chat_send((net_addr)a, "BYE");
        s->state = S_FREE;
        printf("*** sessao com o no %d encerrada\n", a);
    } else if ((strcmp(cmd, "/ping") == 0 || strcmp(cmd, "/uping") == 0) && nargs >= 1) {
        ping_result r;
        do_ping((net_addr)a, nargs >= 2 ? b : 4, nargs >= 3 ? c : 32, cmd[1] == 'p', 0, &r);
        print_ping_summary(&r);
    } else if (strcmp(cmd, "/sessions") == 0) {
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (sessions[i].state != S_FREE)
                printf("  no %d: %s (%s)\n", sessions[i].peer, sessions[i].name,
                       sessions[i].state == S_OPEN ? "aberta" : "aguardando WELCOME");
    } else if (strcmp(cmd, "/stats") == 0) {
        print_stats();
    } else if (strcmp(cmd, "/quit") == 0) {
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (sessions[i].state == S_OPEN) chat_send(sessions[i].peer, "BYE");
        tp_close();
        exit(0);
    } else {
        printf("comandos: /connect <no> | /msg <no> <texto> | <texto> | /bye <no> | "
               "/ping <no> [n] [tam] | /uping <no> [n] [tam] | /sessions | /stats | /quit\n");
    }
    fflush(stdout);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "uso: %s -a <endereco> [-n nome] [-f nodes.txt] [-e] [-v]\n"
            "        [-p perda] [-c corrupcao] [-d atraso_ms] [-j jitter_ms] [-s seed]\n"
            "        [-t timeout_ms] [-r tentativas] [-w espera_pong_ms]\n"
            "        [-b <destino> -N <n> [-L <tamanho>] [-u]]\n", prog);
}

int main(int argc, char **argv) {
    link_config cfg = {0};
    const char *table = "nodes.txt";
    int self = 0, opt, server = 0, tmo = 0, retries = -1;
    int batch_dst = 0, batch_n = 10, batch_size = 32, batch_unreliable = 0;

    while ((opt = getopt(argc, argv, "a:n:f:evp:c:d:j:s:t:r:w:b:N:L:u")) != -1) {
        switch (opt) {
        case 'a': self = atoi(optarg); break;
        case 'n': snprintf(my_name, NAME_MAX, "%.*s", NAME_MAX - 1, optarg); break;
        case 'f': table = optarg; break;
        case 'e': server = 1; break;
        case 'v': g_verbose = 1; break;
        case 'p': cfg.loss = atof(optarg); break;
        case 'c': cfg.corrupt = atof(optarg); break;
        case 'd': cfg.delay_ms = atoi(optarg); break;
        case 'j': cfg.jitter_ms = atoi(optarg); break;
        case 's': cfg.seed = (unsigned)atoi(optarg); break;
        case 't': tmo = atoi(optarg); break;
        case 'r': retries = atoi(optarg); break;
        case 'w': pong_wait_ms = atoi(optarg); break;
        case 'b': batch_dst = atoi(optarg); break;
        case 'N': batch_n = atoi(optarg); break;
        case 'L': batch_size = atoi(optarg); break;
        case 'u': batch_unreliable = 1; break;
        default: usage(argv[0]); return 1;
        }
    }
    if (self <= 0 || self > 254) { usage(argv[0]); return 1; }
    my_addr = (net_addr)self;
    if (strcmp(my_name, "anonimo") == 0) snprintf(my_name, NAME_MAX, "no%d", self);

    if (tp_init(my_addr, table, &cfg, tmo, retries) < 0) return 1;
    tp_bind(CHAT_PORT);
    tp_bind(PING_PORT);
    tp_bind(ECHO_PORT);

    /* ---- modo batch: n pings e uma linha CSV ---- */
    if (batch_dst > 0) {
        ping_result r;
        do_ping((net_addr)batch_dst, batch_n, batch_size, !batch_unreliable, 1, &r);
        const tp_stats *s = tp_get_stats();
        const link_stats *l = &s->net.link;
        printf("%s,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%d,%lu,%d,%.2f,%ld,%ld,%lu,%lu\n",
               batch_unreliable ? "U" : "R", cfg.loss, cfg.corrupt, cfg.delay_ms, cfg.jitter_ms,
               tmo > 0 ? tmo : TP_DEFAULT_TIMEOUT_MS, batch_n, batch_size,
               r.delivered, r.failed, r.retrans, (int)l->bad_checksum,
               r.delivered ? (double)r.rtt_sum / r.delivered : 0.0, r.rtt_max, r.total_ms,
               s->app_bytes_sent + s->app_bytes_recv, l->bytes_sent + l->bytes_recv);
        tp_close();
        return 0;
    }

    /* ---- modo servidor: so atende ---- */
    if (server) {
        printf("no %d (%s) no ar, modo servidor\n", self, my_name);
        fflush(stdout);
        for (;;) service(200);
    }

    /* ---- modo interativo ---- */
    printf("no %d (%s) no ar. /help pra ver os comandos.\n", self, my_name);
    fflush(stdout);
    char line[TP_MTU];
    for (;;) {
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            if (fgets(line, sizeof line, stdin) == NULL) {
                command("/quit");
                return 0;
            }
            command(line);
        }
        service(50);
    }
}
