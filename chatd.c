#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>

#define MAX_CLIENTS     128
#define MAX_NAME_LEN    32
#define MAX_STATUS_LEN  64
#define MAX_MSG_LEN     80
#define ROOM_NAME       "#all"

#define ERR_UNREADABLE  0
#define ERR_NAME_IN_USE 1
#define ERR_UNKNOWN_RCP 2
#define ERR_ILLEGAL_CHR 3
#define ERR_TOO_LONG    4

#define ST_HEADER 0
#define ST_BODY   1

#define HDR_BUFSIZE 64

typedef struct {
    int  fd;
    char name[MAX_NAME_LEN + 1];
    char status[MAX_STATUS_LEN + 1];

    int  state;

    char hdr_buf[HDR_BUFSIZE];
    int  hdr_len;
    int  hdr_pipes;

    char code[8];
    int  body_need;

    char *body;
    int   body_got;
} Client;

static Client clients[MAX_CLIENTS];
static int g_lfd = -1;
static struct pollfd pfds[MAX_CLIENTS + 1];

static void handle_sigint(int sig)
{
    (void)sig;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            close(clients[i].fd);
        }
    }

    if (g_lfd >= 0) {
        close(g_lfd);
    }

    write(STDERR_FILENO, "\nchatd: shutting down.\n", 23);
    _exit(0);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;

    while (sent < len) {
        int n = write(fd, buf + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (n == 0) {
            return -1;
        }

        sent += n;
    }

    return 0;
}

static int send_msg(int fd, const char *from, const char *to, const char *body)
{
    char payload[70000];
    char frame[70100];

    int plen = snprintf(payload, sizeof(payload), "%s|%s|%s|", from, to, body);
    if (plen < 0 || plen >= (int)sizeof(payload)) {
        return -1;
    }

    int flen = snprintf(frame, sizeof(frame), "1|MSG|%d|%s", plen, payload);
    if (flen < 0 || flen >= (int)sizeof(frame)) {
        return -1;
    }

    return send_all(fd, frame, flen);
}

static int send_err(int fd, int code, const char *explanation)
{
    char payload[512];
    char frame[700];

    int plen = snprintf(payload, sizeof(payload), "%d|%s|", code, explanation);
    if (plen < 0 || plen >= (int)sizeof(payload)) {
        return -1;
    }

    int flen = snprintf(frame, sizeof(frame), "1|ERR|%d|%s", plen, payload);
    if (flen < 0 || flen >= (int)sizeof(frame)) {
        return -1;
    }

    return send_all(fd, frame, flen);
}

static void broadcast(const char *from, const char *to, const char *body, int exclude_fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            continue;
        }

        if (clients[i].name[0] == '\0') {
            continue;
        }

        if (clients[i].fd == exclude_fd) {
            continue;
        }

        send_msg(clients[i].fd, from, to, body);
    }
}

static int valid_name_chars(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (!isalnum(c) && c != '-' && c != '_') {
            return 0;
        }
    }

    return 1;
}

static int valid_printable(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 32 || c > 126) {
            return 0;
        }
    }

    return 1;
}

static int valid_digits_only(const char *s)
{
    if (s[0] == '\0') {
        return 0;
    }

    if (strlen(s) > 5) {
        return 0;
    }

    for (int i = 0; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
    }

    return 1;
}

static void init_client(int slot, int fd)
{
    clients[slot].fd = fd;
    clients[slot].name[0] = '\0';
    clients[slot].status[0] = '\0';

    clients[slot].state = ST_HEADER;

    clients[slot].hdr_len = 0;
    clients[slot].hdr_pipes = 0;

    clients[slot].code[0] = '\0';
    clients[slot].body_need = 0;
    clients[slot].body_got = 0;
    clients[slot].body = NULL;
}

static int drop_client(int slot)
{
    Client *c = &clients[slot];

    if (c->fd >= 0) {
        close(c->fd);
    }

    free(c->body);

    c->fd = -1;
    c->body = NULL;
    c->name[0] = '\0';
    c->status[0] = '\0';
    c->state = ST_HEADER;
    c->hdr_len = 0;
    c->hdr_pipes = 0;
    c->body_need = 0;
    c->body_got = 0;

    return 1;
}

#define TAKE_FIELD(dst, dstmax, body, blen, pos)                    \
    do {                                                            \
        int _s = (pos);                                             \
        int _p = (pos);                                             \
        while (_p < (blen) && (body)[_p] != '|') {                  \
            _p++;                                                   \
        }                                                           \
        if (_p >= (blen)) {                                         \
            (pos) = -1;                                             \
        } else {                                                    \
            int _l = _p - _s;                                       \
            if (_l >= (int)(dstmax)) {                              \
                _l = (int)(dstmax) - 1;                             \
            }                                                       \
            memcpy((dst), (body) + _s, _l);                         \
            (dst)[_l] = '\0';                                       \
            (pos) = _p + 1;                                         \
        }                                                           \
    } while (0)

static int find_user_fd(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && strcmp(clients[i].name, name) == 0) {
            return clients[i].fd;
        }
    }

    return -1;
}

static int dispatch(int slot)
{
    Client *c = &clients[slot];
    char *body = c->body;
    int blen = c->body_need;
    int pos = 0;

    if (blen <= 0 || body[blen - 1] != '|') {
        send_err(c->fd, ERR_UNREADABLE, "Unreadable message");
        return 1;
    }

    if (strcmp(c->code, "NAM") == 0) {
        if (c->name[0] != '\0') {
            send_err(c->fd, ERR_NAME_IN_USE, "Name already set");
            return 0;
        }

        char req[MAX_NAME_LEN + 2];
        TAKE_FIELD(req, sizeof(req), body, blen, pos);

        if (pos < 0 || pos != blen) {
            send_err(c->fd, ERR_UNREADABLE, "Malformed NAM");
            return 1;
        }

        int nlen = strlen(req);

        if (nlen == 0 || nlen > MAX_NAME_LEN) {
            send_err(c->fd, ERR_TOO_LONG, "Name too long or empty");
            return 0;
        }

        if (!valid_name_chars(req, nlen)) {
            send_err(c->fd, ERR_ILLEGAL_CHR, "Illegal character in name");
            return 0;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && i != slot && strcmp(clients[i].name, req) == 0) {
                send_err(c->fd, ERR_NAME_IN_USE, "Name already in use");
                return 0;
            }
        }

        strncpy(c->name, req, MAX_NAME_LEN);
        c->name[MAX_NAME_LEN] = '\0';

        send_msg(c->fd, ROOM_NAME, c->name, "Welcome to the chat!");

        return 0;
    }

    if (c->name[0] == '\0') {
        send_err(c->fd, ERR_UNREADABLE, "Must send NAM first");
        return 1;
    }

    if (strcmp(c->code, "SET") == 0) {
        int slen = blen - 1;

        if (slen > MAX_STATUS_LEN) {
            send_err(c->fd, ERR_TOO_LONG, "Status too long");
            return 0;
        }

        char new_status[MAX_STATUS_LEN + 1];
        memcpy(new_status, body, slen);
        new_status[slen] = '\0';

        if (!valid_printable(new_status, slen)) {
            send_err(c->fd, ERR_ILLEGAL_CHR, "Illegal character in status");
            return 0;
        }

        strncpy(c->status, new_status, MAX_STATUS_LEN);
        c->status[MAX_STATUS_LEN] = '\0';

        if (slen > 0) {
            char announcement[256];
            snprintf(announcement, sizeof(announcement), "%s is now \"%s\"", c->name, c->status);
            broadcast(ROOM_NAME, ROOM_NAME, announcement, -1);
        }

        return 0;
    }

    if (strcmp(c->code, "MSG") == 0) {
        char ignored[MAX_NAME_LEN + 2];
        char recipient[MAX_NAME_LEN + 4];

        TAKE_FIELD(ignored, sizeof(ignored), body, blen, pos);
        if (pos < 0) {
            send_err(c->fd, ERR_UNREADABLE, "Malformed MSG");
            return 1;
        }

        TAKE_FIELD(recipient, sizeof(recipient), body, blen, pos);
        if (pos < 0) {
            send_err(c->fd, ERR_UNREADABLE, "Malformed MSG");
            return 1;
        }

        int mlen = blen - pos - 1;

        if (mlen < 1) {
            send_err(c->fd, ERR_UNREADABLE, "Empty message");
            return 1;
        }

        if (mlen > MAX_MSG_LEN) {
            send_err(c->fd, ERR_TOO_LONG, "Message too long");
            return 0;
        }

        char message[MAX_MSG_LEN + 1];
        memcpy(message, body + pos, mlen);
        message[mlen] = '\0';

        if (!valid_printable(message, mlen)) {
            send_err(c->fd, ERR_ILLEGAL_CHR, "Illegal character in message");
            return 0;
        }

        if (strcmp(recipient, ROOM_NAME) == 0) {
            broadcast(c->name, ROOM_NAME, message, -1);
            return 0;
        }

        int recipient_fd = find_user_fd(recipient);
        if (recipient_fd < 0) {
            send_err(c->fd, ERR_UNKNOWN_RCP, "Unknown recipient");
            return 0;
        }

        send_msg(recipient_fd, c->name, recipient, message);
        return 0;
    }

    if (strcmp(c->code, "WHO") == 0) {
        int qlen = blen - 1;

        if (qlen <= 0) {
            send_err(c->fd, ERR_UNREADABLE, "Malformed WHO");
            return 1;
        }

        char query[MAX_NAME_LEN + 4];

        if (qlen >= (int)sizeof(query)) {
            qlen = (int)sizeof(query) - 1;
        }

        memcpy(query, body, qlen);
        query[qlen] = '\0';

        if (strcmp(query, ROOM_NAME) == 0) {
            char result[65000];
            int rlen = 0;

            result[0] = '\0';

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd < 0 || clients[i].name[0] == '\0') {
                    continue;
                }

                if (rlen > 0 && rlen < (int)sizeof(result) - 1) {
                    result[rlen++] = '\n';
                    result[rlen] = '\0';
                }

                int n;

                if (clients[i].status[0] != '\0') {
                    n = snprintf(result + rlen, sizeof(result) - rlen,
                                 "%s: %s", clients[i].name, clients[i].status);
                } else {
                    n = snprintf(result + rlen, sizeof(result) - rlen,
                                 "%s", clients[i].name);
                }

                if (n < 0) {
                    break;
                }

                rlen += n;

                if (rlen >= (int)sizeof(result)) {
                    result[sizeof(result) - 1] = '\0';
                    break;
                }
            }

            send_msg(c->fd, ROOM_NAME, c->name, result);
            return 0;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && strcmp(clients[i].name, query) == 0) {
                char result[256];

                if (clients[i].status[0] != '\0') {
                    snprintf(result, sizeof(result), "%s: %s",
                             clients[i].name, clients[i].status);
                } else {
                    snprintf(result, sizeof(result), "No status");
                }

                send_msg(c->fd, ROOM_NAME, c->name, result);
                return 0;
            }
        }

        send_err(c->fd, ERR_UNKNOWN_RCP, "Unknown user");
        return 0;
    }

    send_err(c->fd, ERR_UNREADABLE, "Unknown message type");
    return 1;
}

static int handle_read(int slot)
{
    Client *c = &clients[slot];

    while (1) {
        if (c->state == ST_HEADER) {
            while (c->hdr_pipes < 3) {
                char ch;
                int r = read(c->fd, &ch, 1);

                if (r == 0) {
                    return drop_client(slot);
                }

                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return 0;
                    }

                    if (errno == EINTR) {
                        continue;
                    }

                    return drop_client(slot);
                }

                if (c->hdr_len >= HDR_BUFSIZE - 1) {
                    send_err(c->fd, ERR_UNREADABLE, "Header too long");
                    return drop_client(slot);
                }

                c->hdr_buf[c->hdr_len++] = ch;

                if (ch == '|') {
                    c->hdr_pipes++;
                }
            }

            c->hdr_buf[c->hdr_len] = '\0';

            char ver[4] = "";
            char code[8] = "";
            char len_s[16] = "";

            if (sscanf(c->hdr_buf, "%3[^|]|%7[^|]|%15[^|]|", ver, code, len_s) != 3) {
                send_err(c->fd, ERR_UNREADABLE, "Malformed header");
                return drop_client(slot);
            }

            if (strcmp(ver, "1") != 0) {
                send_err(c->fd, ERR_UNREADABLE, "Unknown protocol version");
                return drop_client(slot);
            }

            if (strlen(code) != 3) {
                send_err(c->fd, ERR_UNREADABLE, "Bad message code");
                return drop_client(slot);
            }

            if (!valid_digits_only(len_s)) {
                send_err(c->fd, ERR_UNREADABLE, "Invalid body length");
                return drop_client(slot);
            }

            int blen = atoi(len_s);

            if (blen < 0 || blen > 99999) {
                send_err(c->fd, ERR_UNREADABLE, "Invalid body length");
                return drop_client(slot);
            }

            strncpy(c->code, code, sizeof(c->code) - 1);
            c->code[sizeof(c->code) - 1] = '\0';

            free(c->body);
            c->body = malloc(blen + 1);

            if (c->body == NULL) {
                return drop_client(slot);
            }

            c->body[blen] = '\0';
            c->body_need = blen;
            c->body_got = 0;

            c->hdr_len = 0;
            c->hdr_pipes = 0;

            c->state = ST_BODY;
        }

        if (c->state == ST_BODY) {
            int remaining = c->body_need - c->body_got;

            if (remaining == 0) {
                c->state = ST_HEADER;
                int fatal = dispatch(slot);

                if (fatal) {
                    return drop_client(slot);
                }

                continue;
            }

            int r = read(c->fd, c->body + c->body_got, remaining);

            if (r == 0) {
                return drop_client(slot);
            }

            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                }

                if (errno == EINTR) {
                    continue;
                }

                return drop_client(slot);
            }

            c->body_got += r;

            if (c->body_got == c->body_need) {
                c->body[c->body_need] = '\0';
                c->state = ST_HEADER;

                int fatal = dispatch(slot);

                if (fatal) {
                    return drop_client(slot);
                }

                continue;
            }
        }
    }
}

static int rebuild_pfds(int lfd)
{
    pfds[0].fd = lfd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    int n = 1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            pfds[n].fd = clients[i].fd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
    }

    return n;
}

static int slot_for_fd(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            return i;
        }
    }

    return -1;
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            return i;
        }
    }

    return -1;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].body = NULL;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    if (lfd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(lfd);
        return 1;
    }

    if (listen(lfd, 16) < 0) {
        perror("listen");
        close(lfd);
        return 1;
    }

    g_lfd = lfd;

    printf("chatd listening on port %d\n", port);
    fflush(stdout);

    while (1) {
        int nfds = rebuild_pfds(lfd);
        int ready = poll(pfds, nfds, -1);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll");
            break;
        }

        for (int pi = 0; pi < nfds && ready > 0; pi++) {
            if (!(pfds[pi].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }

            ready--;

            if (pfds[pi].fd == lfd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                int cfd = accept(lfd, (struct sockaddr *)&client_addr, &client_len);

                if (cfd < 0) {
                    perror("accept");
                    continue;
                }

                set_nonblocking(cfd);

                int slot = free_slot();

                if (slot < 0) {
                    send_err(cfd, ERR_UNREADABLE, "Server full");
                    close(cfd);
                } else {
                    init_client(slot, cfd);
                }
            } else {
                int slot = slot_for_fd(pfds[pi].fd);

                if (slot >= 0) {
                    handle_read(slot);
                }
            }
        }
    }

    close(lfd);
    return 0;
}


