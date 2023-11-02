#ifndef _PTY_FORK_H_
#define _PTY_FORK_H_

#include <stdlib.h>
#include <termio.h>

char* ptySlaveName();

pid_t ptyFork(int *masterFd, char *slaveName, size_t snLen,
        const struct termios *slaveTermios, const struct winsize *slaveWS);

#endif
