#!/usr/bin/env python3
"""Roda um experimento: sobe um no servidor (eco), roda um no cliente em modo
batch (n pings) com os mesmos parametros de simulacao, e imprime a linha CSV
que o cliente devolve, prefixada com o nome do experimento.

Uso: python3 experimento.py <node> <experimento> <R|U> <N> <L> [opcoes do node]

As opcoes (-p perda, -c corrupcao, -d atraso, -j jitter, -t timeout,
-r tentativas, -w espera_pong) sao passadas aos DOIS nos: os defeitos do
enlace valem nos dois sentidos.
"""
import os
import subprocess
import sys
import time

node, exp, modo, n, tam = sys.argv[1:6]
opts = sys.argv[6:]
here = os.path.dirname(os.path.abspath(__file__))
tabela = os.path.join(here, "nodes.txt")

with open(tabela, "w") as f:
    f.write("1 127.0.0.1 17001\n2 127.0.0.1 17002\n")

servidor = subprocess.Popen([node, "-a", "2", "-e", "-f", tabela, "-s", "1"] + opts,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.2)

cmd = [node, "-a", "1", "-f", tabela, "-s", "2", "-b", "2", "-N", n, "-L", tam] + opts
if modo == "U":
    cmd.append("-u")
saida = subprocess.run(cmd, capture_output=True, text=True).stdout.strip()

servidor.kill()
servidor.wait()
print(f"{exp},{saida}")
