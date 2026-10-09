// Stub di sys/select.h per il fuzzing sul PC (vedi sys/socket.h).
#pragma once
#include <sys/time.h>
typedef struct { int unused; } fd_set;
#define FD_ZERO(s) ((void) (s))
#define FD_SET(fd, s) ((void) (fd), (void) (s))
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
