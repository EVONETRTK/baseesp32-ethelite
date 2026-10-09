// Stub di netdb.h per il fuzzing sul PC (vedi sys/socket.h).
#pragma once
#include <sys/socket.h>
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
int getaddrinfo(const char *host, const char *port, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
