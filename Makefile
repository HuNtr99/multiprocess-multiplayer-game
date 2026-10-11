CC = gcc
CFLAGS = -Wall -Wextra -pthread

all: server client

# Compile server
server:
	$(CC) $(CFLAGS) server.c -o server

# Compile client (used by Player 1, 2, and 3)
client:
	$(CC) $(CFLAGS) client.c -o client

clean:
	rm -f server client game.log scores.txt

.PHONY: all server client clean