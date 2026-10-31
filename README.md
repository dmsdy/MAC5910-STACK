# Mini-Pilha de Rede em Camadas — MAC5910

Trabalho prático de MAC5910 (IME-USP): uma pilha de rede simplificada em C,
com quatro camadas independentes (aplicação, transporte, rede e enlace)
rodando sobre soquetes UDP reais. Cada processo `node` é um nó completo da
rede, identificado por um endereço lógico, e a aplicação é um chat entre
nós com controle de sessão. O enlace simula perda, corrupção e atraso, e o
transporte recupera com ACK e retransmissão.

```
  aplicação   node.c         chat (HELLO/MSG/BYE), eco e ping
  transporte  transport.c    portas lógicas, numeração, ACK/retransmissão, multiplexação
  rede        net.c          endereços lógicos, tabela de nós, pacotes
  enlace      link.c         quadros com checksum, simulação de perda/atraso/corrupção
  ----------  soquete UDP  ----------
```

## Como rodar

Compilar:

```
make
```

Isso gera o executável `node`. A tabela `nodes.txt` diz onde cada nó
está (endereço lógico, host, porta UDP); a que vem no repositório tem
três nós em `127.0.0.1`.

Subir dois nós, cada um num terminal:

```
./node -a 1 -n ana
./node -a 2 -n bia
```

No nó 1:

```
/connect 2          abre sessão com o nó 2 (HELLO / WELCOME)
oi bia              texto puro vai pra todas as sessões abertas
/msg 2 tudo bem?    mensagem pra um nó específico
/ping 2 5 64        5 pings de 64 bytes pelo serviço confiável
/uping 2 5 64       o mesmo pelo serviço não confiável
/sessions           lista as sessões
/stats              contadores de cada camada e overhead
/bye 2              encerra a sessão
/quit
```

### Vendo as camadas trabalhando

Com `-v`, cada camada mostra o que faz com cada mensagem, inclusive os
bytes dos cabeçalhos. É a forma de ver o encapsulamento:

```
$ ./node -a 1 -n ana -v
/connect 2
  [app  ] TX chat -> no 2: HELLO ana
  [tp   ] TX DATA 1:1 -> 2:1 seq=579508467 len=9
  [tp   ] segmento (21 bytes): 01 00 00 01 00 01 22 8a 98 f3 00 09 48 45 4c 4c 4f 20 61 6e 61
  [net  ] TX pacote src=1 dst=2 proto=1 ttl=16 len=21
  [net  ] pacote (29 bytes): 01 01 01 02 10 00 00 15 01 00 00 01 00 01 22 8a 98 f3 00 09 48 45 4c 4c ...
  [link ] TX quadro len=29 checksum=0xe020 -> 127.0.0.1:7002
  [link ] quadro (35 bytes): ab 00 00 1d e0 20 01 01 01 02 10 00 00 15 01 00 00 01 00 01 22 8a 98 f3 ...
  [link ] RX quadro len=20 checksum ok <- 127.0.0.1:7002
  [net  ] RX pacote src=2 dst=1 proto=1 ttl=15 len=12
  [tp   ] RX ACK seq=579508467: confirmado
```

Dá pra ver os 9 bytes de `HELLO ana` virando um segmento de 21, um pacote
de 29 e um quadro de 35 bytes, e o ACK voltando pelo caminho inverso.

### Simulando defeitos no meio físico

Os parâmetros valem pro que o nó *envia*; pra ter defeito nos dois
sentidos, passe nos dois nós.

```
-p 0.2      perde 20% dos quadros
-c 0.1      corrompe um bit em 10% dos quadros (o checksum pega)
-d 50       atraso de 50 ms
-j 20       mais um jitter uniforme de 0 a 20 ms
-s 42       semente, pra repetir o mesmo cenário
-t 200      tempo limite do ACK, em ms (padrão 200)
-r 5        retransmissões máximas (padrão 5)
```

Exemplo: `./node -a 1 -p 0.3 -v` e `./node -a 2 -p 0.3` mostram as
retransmissões e as duplicatas sendo descartadas.

### Outros modos

```
./node -a 2 -e                       servidor: só responde (eco e chat), sem teclado
./node -a 1 -b 2 -N 50 -L 64 [-u]    batch: 50 pings de 64 bytes pro nó 2 e uma linha CSV
```

## Experimentos

Com o `node` compilado:

```
bench/roda_tudo.sh
```

Pra cada cenário, sobe um nó servidor e um cliente em modo batch com os
mesmos parâmetros, e junta tudo em `bench/resultados.csv`. São quatro
experimentos: perda (confiável x não confiável), atraso (com dois tempos
limite), overhead por tamanho de mensagem e corrupção. Um cenário só:

```
python3 bench/experimento.py ./node perda R 50 64 -p 0.2 -w 2000
```

## Arquivos

| Arquivo | O que é |
|---|---|
| `link.h`, `link.c` | Camada de enlace: quadro com checksum sobre UDP e simulação de perda, corrupção e atraso. |
| `net.h`, `net.c` | Camada de rede: endereço lógico, tabela de nós, pacote com TTL. |
| `transport.h`, `transport.c` | Camada de transporte: portas, numeração, ACK e retransmissão (parada-e-espera), multiplexação. |
| `node.c` | Camada de aplicação: chat com sessão, servidor de eco, cliente de ping, modos servidor e batch. |
| `common.h` | Log de depuração (`-v`) e relógio. Não é uma camada. |
| `nodes.txt` | Tabela de nós de exemplo. |
| `Makefile` | Compila o `node`. |
| `bench/experimento.py` | Roda um cenário. Ferramenta de teste, não faz parte da pilha. |
| `bench/roda_tudo.sh` | Roda todos os cenários do relatório. |
| `bench/resultados.csv` | Resultados usados no relatório. |

Os formatos de quadro, pacote, segmento e das mensagens de aplicação
estão em `../docs/PROTOCOL.md` (e no relatório).

## Requisitos

- Linux, `gcc` e `make`.
- Python 3 só pra rodar os experimentos.
