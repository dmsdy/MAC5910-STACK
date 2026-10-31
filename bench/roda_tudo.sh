#!/bin/bash
# Roda todos os experimentos do relatorio e junta tudo em resultados.csv.
# Uso: bench/roda_tudo.sh  (com o node ja compilado em ../node)
cd "$(dirname "$0")"
NODE=../node
N=50
OUT=resultados.csv
echo "experimento,modo,perda,corrupcao,atraso,jitter,timeout,n,tam,entregues,falhas,retrans,checksum_ruim,rtt_medio_ms,rtt_max_ms,tempo_total_ms,bytes_app,bytes_fio" > $OUT

run() { python3 experimento.py $NODE "$@" | tee -a $OUT; }

# 1. perda de pacotes: confiavel x nao confiavel
for p in 0 0.05 0.1 0.2 0.3 0.4 0.5; do
    run perda R $N 64 -p $p -w 2000
    run perda U $N 64 -p $p -w 300
done

# 2. atraso na rede, timeout fixo em 200ms (e depois 600ms pra comparar)
for d in 0 10 25 50 75 100 150 250; do
    run atraso R $N 64 -d $d -t 200 -w 3000
done
for d in 100 150 250; do
    run atraso R $N 64 -d $d -t 600 -w 5000
done
run jitter R $N 64 -d 50 -j 100 -t 200 -w 3000

# 3. overhead das camadas por tamanho de mensagem
for L in 1 8 32 128 512 1024; do
    run overhead R $N $L -w 2000
    run overhead U $N $L -w 300
done

# 4. corrupcao de dados (checksum + retransmissao)
for c in 0 0.05 0.1 0.2 0.3; do
    run corrupcao R $N 64 -c $c -w 2000
    run corrupcao U $N 64 -c $c -w 300
done

rm -f nodes.txt
