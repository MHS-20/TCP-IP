/*
 * Minimal UDP client/server for exercising the stack through liblevelip:
 *
 *   udp echo <port>                   echo every datagram back to its sender
 *   udp send <host> <port> <message>  send one datagram and print the reply
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int echo(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("udp echo: socket/bind");
        return 1;
    }

    printf("echoing on port %d\n", port);
    fflush(stdout);

    for (;;) {
        char buf[65536];
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof(peer);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peerlen);

        if (n < 0) {
            perror("udp echo: recvfrom");
            return 1;
        }

        printf("%zd bytes from %s:%d\n", n, inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        fflush(stdout);

        if (sendto(fd, buf, n, 0, (struct sockaddr *)&peer, peerlen) < 0) {
            perror("udp echo: sendto");
        }
    }
}

static int send_one(const char *host, int port, const char *message)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(port) };

    if (fd < 0 || inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        fprintf(stderr, "udp send: bad socket or address %s\n", host);
        return 1;
    }

    if (sendto(fd, message, strlen(message), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("udp send: sendto");
        return 1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 3000) <= 0) {
        fprintf(stderr, "udp send: no reply within 3s\n");
        return 1;
    }

    char buf[65536];
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if (n < 0) {
        perror("udp send: recvfrom");
        return 1;
    }

    buf[n] = '\0';
    printf("%s\n", buf);
    close(fd);

    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "echo") == 0) return echo(atoi(argv[2]));
    if (argc == 5 && strcmp(argv[1], "send") == 0) return send_one(argv[2], atoi(argv[3]), argv[4]);

    fprintf(stderr, "usage: %s echo <port> | send <host> <port> <message>\n", argv[0]);
    return 2;
}
