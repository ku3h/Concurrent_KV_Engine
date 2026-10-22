#ifndef SERVER_H
#define SERVER_H

#include "hash_table.h"
#include "wal.h"

/* Start the epoll-based TCP server.
 * Binds to the given port, accepts connections, and processes
 * GET/SET/DEL commands from clients.
 * This function blocks until the server is stopped.
 * Returns 0 on clean shutdown, -1 on error. */
int server_run(int port, ConcurrentHashTable *ht, Wal *wal);

#endif
