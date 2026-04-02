# Makefile — Digital Library Reservation Platform
#
# Targets:
#   all      — build both server and client
#   server   — build server only
#   client   — build client only
#   clean    — remove compiled binaries and object files
#   reset    — clean + remove runtime files (books.dat, server.log)
#
# Build:
#   make
#
# Run (two separate terminals):
#   Terminal 1:  ./server
#   Terminal 2:  ./client [server_ip]   (default: 127.0.0.1)

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -g -D_POSIX_C_SOURCE=200809L
LDFLAGS =

# server needs pthreads; client does not
SERVER_LIBS = -lpthread
CLIENT_LIBS =

.PHONY: all server client clean reset

all: server client
	@echo ""
	@echo "Build complete."
	@echo "  Start server : ./server"
	@echo "  Start client : ./client [server_ip]"
	@echo ""

server: server.c protocol.h
	$(CC) $(CFLAGS) -o server server.c $(SERVER_LIBS)
	@echo "  [OK] server built"

client: client.c protocol.h
	$(CC) $(CFLAGS) -o client client.c $(CLIENT_LIBS)
	@echo "  [OK] client built"

clean:
	rm -f server client *.o
	@echo "  [OK] binaries removed"

reset: clean
	rm -f books.dat server.log
	@echo "  [OK] runtime files removed"

