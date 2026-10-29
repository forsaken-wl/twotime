CC ?= cc
CFLAGS ?= -O2 -Wall

azure: src/azure.c
	$(CC) $(CFLAGS) -o azure src/azure.c

inst: inst.c
	$(CC) $(CFLAGS) -o inst inst.c

all: azure inst

clean:
	rm -f azure inst
