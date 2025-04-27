# Make file to compile web server code
# Lachlan Murphy 2025

CC = gcc

CFLAGS = -g -Wall -pthread -lpthread

LIBS = -lcrypto -lssl

default: all

all: server client

server: dfs.c
	$(CC) $(CFLAGS) -o dfs dfs.c array.c $(LIBS)

client: dfc.c
	$(CC) $(CFLAGS) -o dfc dfc.c $(LIBS)