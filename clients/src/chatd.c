/*
 * chatd.c - Simple Internet Chat Server
 * CS 214 Spring 2026 - Project IV
 *
 * Usage: ./chatd <port>
 *
 * Single-threaded server using poll() to multiplex I/O across all
 * connected clients. No mutexes needed — all state is accessed from
 * one thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>

/* ----------------------------- constants ---------------------------------- */
#define MAX_CLIENTS     128
#define MAX_NAME_LEN    32
#define MAX_STATUS_LEN  64
#define MAX_MSG_LEN     80
#define ROOM_NAME       "#all"

/* error codes */
#define ERR_UNREADABLE  0
#define ERR_NAME_IN_USE 1
#define ERR_UNKNOWN_RCP 2
#define ERR_ILLEGAL_CHR 3
#define ERR_TOO_LONG    4

/* per-client parser states */
#define ST_HEADER   0   /* reading version|code|length| */
#define ST_BODY     1   /* reading body bytes            */

#define HDR_BUFSIZE 64

/* ----------------------------- data types --------------------------------- */
typedef struct {
    int  fd;                            /* -1 = slot free               */
    char name[MAX_NAME_LEN + 1];        /* empty string = not yet named */
    char status[MAX_STATUS_LEN + 1];

    /* --- incremental read state machine --- */
    int  state;                         /* ST_HEADER or ST_BODY         */

    char hdr_buf[HDR_BUFSIZE];
    int  hdr_len;
    int  hdr_pipes;                     /* number of '|' chars seen     */

    char code[8];                       /* parsed message code          */
    int  body_need;                     /* total body bytes expected    */

    char *body;                         /* malloc'd, body_need+1 bytes  */
    int   body_got;                     /* bytes received so far        */
} Client;

/* ----------------------------- globals ------------------------------------ */
static Client       clients[MAX_CLIENTS];
static int          g_lfd = -1;   /* listen fd, for signal handler */

static void handle_sigint(int sig)
{
    (void)sig;
    /* Close all client connections, then the listen socket.
     * Writing to stderr is async-signal-safe. */
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0) close(clients[i].fd);
    if (g_lfd >= 0) close(g_lfd);
    write(STDERR_FILENO, "\nchatd: caught SIGINT, shutting down.\n", 38);
    _exit(0);
}
static struct pollfd pfds[MAX_CLIENTS + 1]; /* pfds[0] = listen fd      */

/* ----------------------------- low-level I/O ------------------------------ */

static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = write(fd, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int send_msg(int fd, const char *from, const char *to, const char *body)
{
    char payload[70000];
    int  plen = snprintf(payload, sizeof(payload), "%s|%s|%s|", from, to, body);
    if (plen < 0 || plen >= (int)sizeof(payload)) return -1;
    char frame[70100];
    int  flen = snprintf(frame, sizeof(frame), "1|MSG|%d|%s", plen, payload);
    if (flen < 0 || flen >= (int)sizeof(frame)) return -1;
    return send_all(fd, frame, flen);
}

static int send_err(int fd, int code, const char *explanation)
{
    char payload[300];
    int  plen = snprintf(payload, sizeof(payload), "%d|%s|", code, explanation);
    char frame[400];
    int  flen = snprintf(frame, sizeof(frame), "1|ERR|%d|%s", plen, payload);
    return send_all(fd, frame, flen);
}

/* Broadcast MSG to every named client; pass exclude_fd=-1 for no exclusion. */
static void broadcast(const char *from, const char *to,
                      const char *body, int exclude_fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0 || clients[i].name[0] == '\0') continue;
        if (clients[i].fd == exclude_fd) continue;
        send_msg(clients[i].fd, from, to, body);
    }
}

/* ----------------------------- validation --------------------------------- */

static int valid_name_chars(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return 0;
    }
    return 1;
}

static int valid_printable(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) return 0;
    }
    return 1;
}

/* ----------------------------- client lifecycle --------------------------- */

static void init_client(int slot, int fd)
{
    clients[slot].fd        = fd;
    clients[slot].name[0]   = '\0';
    clients[slot].status[0] = '\0';
    clients[slot].state     = ST_HEADER;
    clients[slot].hdr_len   = 0;
    clients[slot].hdr_pipes = 0;
    clients[slot].body      = NULL;
    clients[slot].body_need = 0;
    clients[slot].body_got  = 0;
}

/* Close, announce, and free a slot. Always returns 1 for convenience. */
static int drop_client(int slot)
{
    Client *c = &clients[slot];
    if (c->name[0] != '\0') {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s has left the chat.", c->name);
        broadcast(ROOM_NAME, ROOM_NAME, msg, c->fd);
    }
    close(c->fd);
    free(c->body);
    c->fd     = -1;
    c->body   = NULL;
    c->name[0]= '\0';
    return 1;
}

/* ----------------------------- message dispatch --------------------------- */

/*
 * Called once a full message body has been accumulated.
 * Returns 0 on success; returns 1 if the connection should be closed
 * (fatal error 0 was sent, or the client misbehaved irrecoverably).
 */
static int dispatch(int slot)
{
    Client     *c    = &clients[slot];
    const char *code = c->code;
    char       *body = c->body;
    int         blen = c->body_need;

    if (blen == 0 || body[blen - 1] != '|') {
        send_err(c->fd, ERR_UNREADABLE, "Message must end with '|'");
        return 1;
    }

    /*
     * TAKE_FIELD(dst, dstmax, pos):
     *   Reads the next '|'-terminated token from body[] starting at pos.
     *   Copies up to dstmax-1 bytes into dst and NUL-terminates.
     *   Evaluates to the new pos (after the '|'), or -1 on error.
     */
#define TAKE_FIELD(dst, dstmax, pos) ({                         \
    int _s = (pos), _p = (pos);                                 \
    while (_p < blen && body[_p] != '|') _p++;                 \
    if (_p >= blen) { _p = -1; }                               \
    else {                                                       \
        int _l = _p - _s;                                       \
        if (_l >= (int)(dstmax)) _l = (int)(dstmax) - 1;       \
        memcpy((dst), body + _s, _l);                           \
        (dst)[_l] = '\0';                                        \
        _p++;                                                    \
    }                                                            \
    _p; })

    int pos = 0;

    /* ==================== NAM ==================== */
    if (strcmp(code, "NAM") == 0) {
        if (c->name[0] != '\0') {
            send_err(c->fd, ERR_UNREADABLE, "Already named");
            return 0;
        }
        char req[MAX_NAME_LEN + 2] = "";
        pos = TAKE_FIELD(req, sizeof(req), pos);
        if (pos < 0) { send_err(c->fd, ERR_UNREADABLE, "Malformed NAM"); return 1; }

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
            if (clients[i].fd >= 0 && i != slot &&
                strcmp(clients[i].name, req) == 0) {
                send_err(c->fd, ERR_NAME_IN_USE, "Name already in use");
                return 0;
            }
        }
        strncpy(c->name, req, MAX_NAME_LEN);
        c->name[MAX_NAME_LEN] = '\0';
        send_msg(c->fd, ROOM_NAME, req, "Welcome to the chat!");
        char ann[128];
        snprintf(ann, sizeof(ann), "%s has joined the chat.", req);
        broadcast(ROOM_NAME, ROOM_NAME, ann, c->fd);

    /* ==================== SET ==================== */
    } else if (strcmp(code, "SET") == 0) {
        if (c->name[0] == '\0') {
            send_err(c->fd, ERR_UNREADABLE, "Must set name first");
            return 0;
        }
        /* status occupies body[pos .. blen-2]; blen-1 is the trailing '|' */
        int slen = blen - pos - 1;
        if (slen < 0) slen = 0;
        if (slen > MAX_STATUS_LEN) {
            send_err(c->fd, ERR_TOO_LONG, "Status too long");
            return 0;
        }
        char new_status[MAX_STATUS_LEN + 1];
        memcpy(new_status, body + pos, slen);
        new_status[slen] = '\0';
        if (!valid_printable(new_status, slen)) {
            send_err(c->fd, ERR_ILLEGAL_CHR, "Illegal character in status");
            return 0;
        }
        strncpy(c->status, new_status, MAX_STATUS_LEN);
        c->status[MAX_STATUS_LEN] = '\0';
        if (slen > 0) {
            char ann[256];
            snprintf(ann, sizeof(ann), "%s is now \"%s\"", c->name, new_status);
            broadcast(ROOM_NAME, ROOM_NAME, ann, -1);
        }

    /* ==================== MSG ==================== */
    } else if (strcmp(code, "MSG") == 0) {
        if (c->name[0] == '\0') {
            send_err(c->fd, ERR_UNREADABLE, "Must set name first");
            return 0;
        }
        char ignored[MAX_NAME_LEN + 2] = "";
        char recip[MAX_NAME_LEN + 4]   = "";
        pos = TAKE_FIELD(ignored, sizeof(ignored), pos);
        if (pos < 0) { send_err(c->fd, ERR_UNREADABLE, "Malformed MSG"); return 1; }
        pos = TAKE_FIELD(recip, sizeof(recip), pos);
        if (pos < 0) { send_err(c->fd, ERR_UNREADABLE, "Malformed MSG"); return 1; }

        int mlen = blen - pos - 1;
        if (mlen < 1) { send_err(c->fd, ERR_UNREADABLE, "Empty body"); return 0; }
        if (mlen > MAX_MSG_LEN) { send_err(c->fd, ERR_TOO_LONG, "Message too long"); return 0; }

        char mbody[MAX_MSG_LEN + 1];
        memcpy(mbody, body + pos, mlen);
        mbody[mlen] = '\0';
        if (!valid_printable(mbody, mlen)) {
            send_err(c->fd, ERR_ILLEGAL_CHR, "Illegal character in message");
            return 0;
        }
        if (strcmp(recip, ROOM_NAME) == 0) {
            broadcast(c->name, ROOM_NAME, mbody, -1);
        } else {
            int rfd = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd >= 0 && strcmp(clients[i].name, recip) == 0) {
                    rfd = clients[i].fd; break;
                }
            }
            if (rfd < 0) { send_err(c->fd, ERR_UNKNOWN_RCP, "Unknown recipient"); return 0; }
            send_msg(rfd, c->name, recip, mbody);
        }

    /* ==================== WHO ==================== */
    } else if (strcmp(code, "WHO") == 0) {
        if (c->name[0] == '\0') {
            send_err(c->fd, ERR_UNREADABLE, "Must set name first");
            return 0;
        }
        int qlen = blen - pos - 1;
        if (qlen < 0) qlen = 0;
        char query[MAX_NAME_LEN + 4];
        if (qlen >= (int)sizeof(query)) qlen = (int)sizeof(query) - 1;
        memcpy(query, body + pos, qlen);
        query[qlen] = '\0';

        if (strcmp(query, ROOM_NAME) == 0) {
            char result[65000];
            int  rlen = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd < 0 || clients[i].name[0] == '\0') continue;
                if (rlen > 0 && rlen < (int)sizeof(result) - 1)
                    result[rlen++] = '\n';
                int n;
                if (clients[i].status[0] != '\0')
                    n = snprintf(result + rlen, sizeof(result) - rlen,
                                 "%s: %s", clients[i].name, clients[i].status);
                else
                    n = snprintf(result + rlen, sizeof(result) - rlen,
                                 "%s", clients[i].name);
                if (n > 0) rlen += n;
            }
            result[rlen] = '\0';
            send_msg(c->fd, ROOM_NAME, c->name, result);
        } else {
            int found = 0;
            char uname[MAX_NAME_LEN + 1]   = "";
            char ustatus[MAX_STATUS_LEN + 1]= "";
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd >= 0 &&
                    strcmp(clients[i].name, query) == 0) {
                    found = 1;
                    strncpy(uname,   clients[i].name,   MAX_NAME_LEN);
                    strncpy(ustatus, clients[i].status, MAX_STATUS_LEN);
                    break;
                }
            }
            if (!found) { send_err(c->fd, ERR_UNKNOWN_RCP, "Unknown user"); return 0; }
            char result[256];
            if (ustatus[0] != '\0')
                snprintf(result, sizeof(result), "%s: %s", uname, ustatus);
            else
                snprintf(result, sizeof(result), "No status");
            send_msg(c->fd, ROOM_NAME, c->name, result);
        }

    /* ==================== unknown ==================== */
    } else {
        send_err(c->fd, ERR_UNREADABLE, "Unknown message type");
        return 1;
    }

    return 0;
#undef TAKE_FIELD
}

/* ----------------------------- incremental reader ------------------------- */

/*
 * Called when poll() signals POLLIN for clients[slot].
 * Reads as many bytes as are available right now, advancing the state machine.
 * Returns 1 if the slot was dropped (do not touch it afterward).
 */
static int handle_read(int slot)
{
    Client *c = &clients[slot];

    /* ---------- ST_HEADER: accumulate bytes until we have 3 pipes ---------- */
    if (c->state == ST_HEADER) {
        while (c->hdr_pipes < 3) {
            char ch;
            int r = read(c->fd, &ch, 1);
            if (r == 0) return drop_client(slot);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return drop_client(slot);
            }
            if (c->hdr_len >= HDR_BUFSIZE - 1) {
                send_err(c->fd, ERR_UNREADABLE, "Header too long");
                return drop_client(slot);
            }
            c->hdr_buf[c->hdr_len++] = ch;
            if (ch == '|') c->hdr_pipes++;
        }
        c->hdr_buf[c->hdr_len] = '\0';

        /* Parse "ver|code|len|" */
        char ver[4] = "", code[8] = "", len_s[16] = "";
        if (sscanf(c->hdr_buf, "%3[^|]|%7[^|]|%15[^|]|", ver, code, len_s) != 3) {
            send_err(c->fd, ERR_UNREADABLE, "Malformed header");
            return drop_client(slot);
        }
        if (ver[0] != '1' || ver[1] != '\0') {
            send_err(c->fd, ERR_UNREADABLE, "Unknown protocol version");
            return drop_client(slot);
        }
        int blen = atoi(len_s);
        if (blen < 0 || blen > 99999) {
            send_err(c->fd, ERR_UNREADABLE, "Invalid body length");
            return drop_client(slot);
        }

        strncpy(c->code, code, sizeof(c->code) - 1);
        c->code[sizeof(c->code) - 1] = '\0';
        c->body_need = blen;
        c->body_got  = 0;
        free(c->body);
        c->body = malloc(blen + 1);
        if (!c->body) return drop_client(slot);
        c->body[blen] = '\0';

        /* Reset header accumulators for next message */
        c->hdr_len   = 0;
        c->hdr_pipes = 0;

        if (blen == 0) {
            int fatal = dispatch(slot);
            return fatal ? drop_client(slot) : 0;
        }
        c->state = ST_BODY;
    }

    /* ---------- ST_BODY: bulk-read the rest of the body -------------------- */
    if (c->state == ST_BODY) {
        int remaining = c->body_need - c->body_got;
        int r = read(c->fd, c->body + c->body_got, remaining);
        if (r == 0) return drop_client(slot);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return drop_client(slot);
        }
        c->body_got += r;
        if (c->body_got == c->body_need) {
            c->state = ST_HEADER;
            int fatal = dispatch(slot);
            return fatal ? drop_client(slot) : 0;
        }
    }
    return 0;
}

/* ----------------------------- poll helpers ------------------------------- */

static int rebuild_pfds(int lfd)
{
    pfds[0].fd     = lfd;
    pfds[0].events = POLLIN;
    int n = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) continue;
        pfds[n].fd      = clients[i].fd;
        pfds[n].events  = POLLIN;
        pfds[n].revents = 0;
        n++;
    }
    return n;
}

static int slot_for_fd(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd == fd) return i;
    return -1;
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd < 0) return i;
    return -1;
}

/* ----------------------------- main --------------------------------------- */

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
        clients[i].fd   = -1;
        clients[i].body = NULL;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port),
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }
    g_lfd = lfd;

    printf("chatd listening on port %d\n", port);

    while (1) {
        int nfds  = rebuild_pfds(lfd);
        int ready = poll(pfds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }

        for (int pi = 0; pi < nfds && ready > 0; pi++) {
            if (!(pfds[pi].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ready--;

            if (pfds[pi].fd == lfd) {
                struct sockaddr_in ca;
                socklen_t cal = sizeof(ca);
                int cfd = accept(lfd, (struct sockaddr *)&ca, &cal);
                if (cfd < 0) { perror("accept"); continue; }

                int sl = free_slot();
                if (sl < 0) {
                    send_err(cfd, ERR_UNREADABLE, "Server full");
                    close(cfd);
                } else {
                    init_client(sl, cfd);
                }
            } else {
                int sl = slot_for_fd(pfds[pi].fd);
                if (sl >= 0) handle_read(sl);
            }
        }
    }

    close(lfd);
    return 0;
}
