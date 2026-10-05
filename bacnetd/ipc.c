#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

int ipc_server_start(const char *socket_path)
{
    int fd;
    struct sockaddr_un addr;

    /* Remove stale socket file */
    unlink(socket_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    fprintf(stdout, "[ipc] Listening on %s\n", socket_path);
    return fd;
}

int ipc_accept(int server_fd)
{
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
    }
    return client_fd;
}

int ipc_readline(int fd, char *buf, size_t max)
{
    size_t pos = 0;
    char c;
    ssize_t n;

    while (pos < max - 1) {
        n = read(fd, &c, 1);
        if (n <= 0) {
            return (int)n; /* EOF or error */
        }
        if (c == '\n') {
            break;
        }
        buf[pos++] = c;
    }

    buf[pos] = '\0';
    return (int)pos;
}

int ipc_writeline(int fd, const char *json)
{
    size_t len = strlen(json);
    char *buf = malloc(len + 2);
    if (!buf) return -1;

    memcpy(buf, json, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';

    ssize_t written = write(fd, buf, len + 1);
    free(buf);
    return (written < 0) ? -1 : 0;
}

