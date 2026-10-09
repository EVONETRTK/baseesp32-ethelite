// unistd.h della libreria C di Windows; close() dei socket finti va al
// bersaglio del fuzzing (fuzz_ntrip.c), non alla libreria.
#pragma once
#include_next <unistd.h>
int fuzz_socket_close(int fd);
#define close(fd) fuzz_socket_close(fd)
