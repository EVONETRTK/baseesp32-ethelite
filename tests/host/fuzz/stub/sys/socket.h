// Stub di sys/socket.h (lwIP) per il fuzzing sul PC: le funzioni sono finte e
// le definisce il bersaglio (fuzz_ntrip.c), che fa arrivare le risposte del
// caster dall'input. Solo per le prove, mai nel firmware.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h> // struct timeval

typedef uint32_t socklen_t;
struct sockaddr { uint16_t sa_family; char sa_data[14]; };
struct linger { int l_onoff; int l_linger; };

#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xfff
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_LINGER 0x0080
#define SO_KEEPALIVE 0x0008
#define SO_ERROR 0x1007

int socket(int domain, int type, int protocol);
int connect(int s, const struct sockaddr *addr, socklen_t len);
int setsockopt(int s, int level, int name, const void *val, socklen_t len);
int getsockopt(int s, int level, int name, void *val, socklen_t *len);
int send(int s, const void *data, size_t len, int flags);
int recv(int s, void *mem, size_t len, int flags);
