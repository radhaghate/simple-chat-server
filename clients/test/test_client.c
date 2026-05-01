/*
 * test_client.c - Simple test client for chatd
 * CS 214 Spring 2026 - Project IV
 *
 * Usage: ./test_client <host> <port> <name> [commands...]
 *
 * Runs scripted commands from stdin or argv against the chat server.
 * This is used for automated testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_MSG_LEN 80

static int g_fd = -1;

static int send_all(int fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = write(fd, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Send a raw formatted message */
static void send_raw(const char *code, const char *body_content) {
    /* body_content should NOT include trailing '|' – we add it */
    int blen = strlen(body_content) + 1; /* +1 for trailing '|' */
    char frame[65600];
    int flen = snprintf(frame, sizeof(frame), "1|%s|%d|%s|", code, blen, body_content);
    send_all(g_fd, frame, flen);
    printf("[SENT]  1|%s|%d|%s|\n", code, blen, body_content);
}

/* Read and print one message from server */
static void recv_one(void) {
    char buf[65600];

    /* read until we have "1|XXX|NNNNN|<body>" */
    /* Simple approach: read byte by byte for header, then bulk for body */
    char c;
    char header[64];
    int  hi = 0;
    int  pipes = 0;

    /* Read up to and including the 3rd pipe (the one after length) */
    while (pipes < 3 && hi < 63) {
        if (read(g_fd, &c, 1) <= 0) return;
        header[hi++] = c;
        if (c == '|') pipes++;
    }
    header[hi] = '\0';

    /* Parse: ver|code|len| */
    char ver[4], code[8], len_s[16];
    if (sscanf(header, "%[^|]|%[^|]|%[^|]|", ver, code, len_s) != 3) {
        printf("[RECV ERROR] Could not parse header: %s\n", header);
        return;
    }

    int blen = atoi(len_s);
    if (blen < 0 || blen > 65000) {
        printf("[RECV ERROR] Bogus body length %d\n", blen);
        return;
    }

    int got_bytes = 0;
    while (got_bytes < blen) {
        int r = read(g_fd, buf + got_bytes, blen - got_bytes);
        if (r <= 0) break;
        got_bytes += r;
    }
    buf[got_bytes] = '\0';
    printf("[RECV]  1|%s|%d|%s\n", code, blen, buf);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <host> <port> <name>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int         port = atoi(argv[2]);
    const char *name = argv[3];

    /* Connect */
    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "Unknown host: %s\n", host); return 1; }

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(g_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect"); return 1;
    }
    printf("[INFO] Connected to %s:%d\n", host, port);

    /* Step 1: send NAM */
    send_raw("NAM", name);
    recv_one(); /* welcome or ERR */

    /* Step 2: read scripted commands from stdin */
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "MSG ", 4) == 0) {
            /* format: MSG <recipient> <body> */
            char recipient[64], body[MAX_MSG_LEN + 2];
            if (sscanf(line + 4, "%63s %79[^\n]", recipient, body) == 2) {
                char payload[256];
                snprintf(payload, sizeof(payload), "|%s|%s", recipient, body);
                send_raw("MSG", payload);
                recv_one();
            }
        } else if (strncmp(line, "WHO ", 4) == 0) {
            send_raw("WHO", line + 4);
            recv_one();
        } else if (strncmp(line, "SET ", 4) == 0) {
            send_raw("SET", line + 4);
            recv_one();
        } else if (strcmp(line, "QUIT") == 0) {
            break;
        } else {
            printf("[INFO] Unknown command: %s\n", line);
        }
    }

    close(g_fd);
    return 0;
}
