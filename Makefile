CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g
TARGET = chatd

all: $(TARGET)

$(TARGET): chatd.c
	$(CC) $(CFLAGS) -o $(TARGET) chatd.c

test_client: test/test_client.c
	$(CC) $(CFLAGS) -o test/test_client test/test_client.c

test: all test_client
	chmod +x test/test_chatd.sh
	./test/test_chatd.sh

clean:
	rm -f $(TARGET)
	rm -f test/test_client
	rm -f *.o
	rm -f test/*.o
	rm -f test/bob_output.txt
	rm -f test/server_output.txt
	rm -rf chatd.dSYM
	rm -rf test/test_client.dSYM

.PHONY: all test clean 