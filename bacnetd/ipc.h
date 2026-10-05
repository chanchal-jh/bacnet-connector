#ifndef IPC_H
#define IPC_H

#include <stddef.h>

#define IPC_SOCKET_PATH "/tmp/bacnetd.sock"
#define IPC_BUF_SIZE    4096

/* Start Unix domain socket server, returns fd or -1 on error */
int ipc_server_start(const char *socket_path);

/* Accept one client connection, returns client fd or -1 */
int ipc_accept(int server_fd);

/* Read a newline-terminated JSON line from fd into buf (null-terminated).
 * Returns bytes read, 0 on EOF, -1 on error. */
int ipc_readline(int fd, char *buf, size_t max);

/* Write a newline-terminated JSON line to fd.
 * Returns 0 on success, -1 on error. */
int ipc_writeline(int fd, const char *json);

#endif /* IPC_H */

