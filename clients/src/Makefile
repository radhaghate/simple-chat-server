CC      = gcc
CFLAGS  = -Wall -Wextra -g

TARGETS = chatd test_client

.PHONY: all clean test

all: $(TARGETS)

chatd: chatd.c
	$(CC) $(CFLAGS) -o $@ $<

test_client: test_client.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	bash test_chatd.sh

clean:
	rm -f $(TARGETS)
