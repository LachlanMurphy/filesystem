# Make file to compile web server code
# Lachlan Murphy 2025

CC = gcc

CFLAGS = -g -Wall -pthread -lpthread

default: all

all: server client

server: dfs.c
	$(CC) $(CFLAGS) -o dfs dfs.c array.c

client: dfc.c
	$(CC) $(CFLAGS) -o dfc dfc.c