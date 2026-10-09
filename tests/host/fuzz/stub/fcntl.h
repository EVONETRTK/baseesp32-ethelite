// fcntl.h della libreria C di Windows piu' quanto usa net_util.h (lwIP).
#pragma once
#include_next <fcntl.h>
#define F_GETFL 3
#define F_SETFL 4
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
int fcntl(int fd, int cmd, ...);
