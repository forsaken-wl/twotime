CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

azure: src/azure.c
	$(CC) $(CFLAGS) -o azure src/azure.c $(LDFLAGS)

clean:
	rm -f azure

install: azure
	install -D -m 0755 azure /usr/bin/azure
