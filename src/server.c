#include "server.h"
#include "hash_table.h"
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_EVENTS 64
#define BUF_SIZE 4096
#define BACKLOG 128

/* Per-connection state */
typedef struct {
    int fd;
    char rbuf[BUF_SIZE];
    size_t rlen;
} ConnState;

/* ---------- Non-blocking socket setup ---------- */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- Command parsing ---------- */

/* Parses a single command line from the client buffer.
 * Executes it against the hash table and WAL.
 * Writes the response into `resp` (BUF_SIZE max).
 * Returns the number of bytes consumed from `input`, or 0 if no full line yet. */
static int parse_and_execute(const char *input, size_t len,
                             ConcurrentHashTable *ht, Wal *wal,
                             char *resp, size_t resp_cap) {
    /* Find newline */
    const char *nl = memchr(input, '\n', len);
    if (!nl) return 0;
    size_t line_len = nl - input;

    /* Make a null-terminated copy */
    char line[BUF_SIZE];
    if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
    memcpy(line, input, line_len);
    line[line_len] = '\0';

    /* Strip trailing \r */
    if (line_len > 0 && line[line_len - 1] == '\r') {
        line[--line_len] = '\0';
    }

    /* Parse command */
    char cmd[8];
    char key[512];
    char value[BUF_SIZE];

    if (sscanf(line, "%7s %511s", cmd, key) < 2) {
        /* Check for PING */
        if (strcasecmp(cmd, "PING") == 0 || strcasecmp(line, "PING") == 0) {
            snprintf(resp, resp_cap, "PONG\n");
        } else {
            snprintf(resp, resp_cap, "ERR unknown command\n");
        }
        return line_len + 1; /* consume including newline */
    }

    if (strcasecmp(cmd, "GET") == 0) {
        char *val = ht_get(ht, key);
        if (val) {
            snprintf(resp, resp_cap, "OK %s\n", val);
            free(val);
        } else {
            snprintf(resp, resp_cap, "NIL\n");
        }
    } else if (strcasecmp(cmd, "SET") == 0) {
        /* Extract value: everything after key */
        char *p = line + strlen(cmd) + 1; /* skip "SET " */
        p += strlen(key) + 1;             /* skip "key " */
        if (p < line + line_len) {
            strncpy(value, p, sizeof(value) - 1);
            value[sizeof(value) - 1] = '\0';
            ht_put(ht, key, value);
            wal_append(wal, WAL_OP_PUT, key, value);
            snprintf(resp, resp_cap, "OK\n");
        } else {
            snprintf(resp, resp_cap, "ERR missing value\n");
        }
    } else if (strcasecmp(cmd, "DEL") == 0) {
        int rc = ht_delete(ht, key);
        wal_append(wal, WAL_OP_DEL, key, NULL);
        wal_flush(wal);
        snprintf(resp, resp_cap, rc ? "OK\n" : "NIL\n");
    } else if (strcasecmp(cmd, "COUNT") == 0) {
        snprintf(resp, resp_cap, "OK %d\n", ht_count(ht));
    } else {
        snprintf(resp, resp_cap, "ERR unknown command: %s\n", cmd);
    }

    return line_len + 1; /* consume including newline */
}

/* ---------- Server main loop ---------- */

int server_run(int port, ConcurrentHashTable *ht, Wal *wal) {
    /* Create listening socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    set_nonblocking(listen_fd);

    /* Create epoll instance */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(listen_fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; /* NULL ptr = listening socket */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl");
        close(epfd);
        close(listen_fd);
        return -1;
    }

    printf("Server listening on port %d\n", port);
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* New connection on listening socket */
                for (;;) {
                    struct sockaddr_in cli_addr;
                    socklen_t cli_len = sizeof(cli_addr);
                    int conn_fd = accept(listen_fd,
                                        (struct sockaddr *)&cli_addr, &cli_len);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept");
                        break;
                    }

                    set_nonblocking(conn_fd);

                    ConnState *cs = malloc(sizeof(ConnState));
                    cs->fd = conn_fd;
                    cs->rlen = 0;

                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.ptr = cs;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);
                }
            } else {
                /* Data from a client connection */
                ConnState *cs = (ConnState *)events[i].data.ptr;

                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, cs->fd, NULL);
                    close(cs->fd);
                    free(cs);
                    continue;
                }

                for (;;) {
                    ssize_t bytes = read(cs->fd, cs->rbuf + cs->rlen,
                                        sizeof(cs->rbuf) - cs->rlen - 1);
                    if (bytes > 0) {
                        cs->rlen += bytes;
                        cs->rbuf[cs->rlen] = '\0';

                        /* Process all complete lines */
                        size_t consumed_total = 0;
                        while (consumed_total < cs->rlen) {
                            char resp[BUF_SIZE];
                            int consumed = parse_and_execute(
                                cs->rbuf + consumed_total,
                                cs->rlen - consumed_total,
                                ht, wal, resp, sizeof(resp));

                            if (consumed == 0) break;

                            /* Send response */
                            size_t resp_len = strlen(resp);
                            ssize_t sent = write(cs->fd, resp, resp_len);
                            (void)sent; /* best-effort write */

                            consumed_total += consumed;
                        }

                        /* Shift unconsumed data to front of buffer */
                        if (consumed_total > 0) {
                            memmove(cs->rbuf, cs->rbuf + consumed_total,
                                   cs->rlen - consumed_total);
                            cs->rlen -= consumed_total;
                        }

                        /* Buffer full but no complete line — reset */
                        if (cs->rlen >= sizeof(cs->rbuf) - 1) {
                            cs->rlen = 0;
                        }
                    } else {
                        if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                            /* Connection closed */
                            epoll_ctl(epfd, EPOLL_CTL_DEL, cs->fd, NULL);
                            close(cs->fd);
                            free(cs);
                        }
                        break;
                    }
                }
            }
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
