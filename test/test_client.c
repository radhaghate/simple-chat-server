#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define MAX_MSG_LEN 80

static int g_fd = -1;

static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;

    while (sent < len) {
        int n = write(fd, buf + sent, len - sent);

        if (n <= 0) {
            return -1;
        }

        sent += n;
    }

    return 0;
}

static void send_raw(const char *code, const char *body_content)
{
    int blen = strlen(body_content) + 1;

    char frame[65600];

    int flen = snprintf(frame, sizeof(frame),
                        "1|%s|%d|%s|", code, blen, body_content);

    send_all(g_fd, frame, flen);

    printf("[SENT]  1|%s|%d|%s|\n", code, blen, body_content);
    fflush(stdout);
}

static void recv_one(void)
{
    char buf[65600];
    char header[64];

    char c;
    int hi = 0;
    int pipes = 0;

    while (pipes < 3 && hi < 63) {
        int r = read(g_fd, &c, 1);

        if (r <= 0) {
            printf("[INFO] server closed connection\n");
            return;
        }

        header[hi++] = c;

        if (c == '|') {
            pipes++;
        }
    }

    header[hi] = '\0';

    char ver[4];
    char code[8];
    char len_s[16];

    if (sscanf(header, "%3[^|]|%7[^|]|%15[^|]|", ver, code, len_s) != 3) {
        printf("[RECV ERROR] Could not parse header: %s\n", header);
        return;
    }

    int blen = atoi(len_s);

    if (blen < 0 || blen > 65000) {
        printf("[RECV ERROR] Bad body length: %d\n", blen);
        return;
    }

    int got = 0;

    while (got < blen) {
        int r = read(g_fd, buf + got, blen - got);

        if (r <= 0) {
            break;
        }

        got += r;
    }

    buf[got] = '\0';

    printf("[RECV]  %s|%s|%d|%s\n", ver, code, blen, buf);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <host> <port> <name>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *name = argv[3];

    struct hostent *he = gethostbyname(host);

    if (he == NULL) {
        fprintf(stderr, "Unknown host: %s\n", host);
        return 1;
    }

    g_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (g_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in sa;

    memset(&sa, 0, sizeof(sa));

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(g_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        close(g_fd);
        return 1;
    }

    printf("[INFO] Connected to %s:%d\n", host, port);
    fflush(stdout);

    send_raw("NAM", name);
    recv_one();

    char line[256];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "MSG ", 4) == 0) {
            char recipient[64];
            char body[MAX_MSG_LEN + 2];

            if (sscanf(line + 4, "%63s %80[^\n]", recipient, body) == 2) {
                char payload[256];

                snprintf(payload, sizeof(payload), "|%s|%s", recipient, body);

                send_raw("MSG", payload);
            } else {
                printf("[INFO] Usage: MSG <recipient> <message>\n");
            }
        } else if (strncmp(line, "WHO ", 4) == 0) {
            send_raw("WHO", line + 4);
            recv_one();
        } else if (strncmp(line, "SET ", 4) == 0) {
            send_raw("SET", line + 4);
        } else if (strcmp(line, "QUIT") == 0) {
            break;
        } else {
            printf("[INFO] Unknown command: %s\n", line);
        }

        fflush(stdout);
    }

    close(g_fd);
    return 0;
}
