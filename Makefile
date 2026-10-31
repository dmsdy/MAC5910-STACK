CC = gcc
CFLAGS = -Wall -Wextra -O2
SRC = transport.c net.c link.c
HDR = transport.h net.h link.h common.h

all: node

node: node.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o node node.c $(SRC)

clean:
	rm -f node

.PHONY: all clean
