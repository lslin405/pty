#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <termio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "pty_master_open.h"
#include "pty_fork.h"

#define MAX_SNAME   1000
static char slname[MAX_SNAME];

char* ptySlaveName()
{
    return slname;
}

pid_t ptyFork(int *masterFd, char *slaveName, size_t snLen,
        const struct termios *slaveTermios, const struct winsize *slaveWS)
{
    int mfd, slaveFd, savedErrno;
    pid_t childPid;
    //char slname[MAX_SNAME];

    mfd = ptyMasterOpen(slname, MAX_SNAME);
    if(mfd == -1) return -1;

    if(slaveName != NULL) {
        if(strlen(slname) < snLen) {
            strncpy(slaveName, slname, snLen);

        } else {
            close(mfd);
            errno = EOVERFLOW;
            return -1;
        }
    }

    childPid = fork();
    if(childPid == -1) {
        savedErrno = errno;
        close(mfd);
        errno = savedErrno;
        return -1;
    }

    if(childPid != 0) {
        *masterFd = mfd;
        return childPid;
    }

    if(setsid() == -1) {
        perror("ptyFork: setsid");
        exit(1);
    }

    close(mfd);
    slaveFd = open(slname, O_RDWR);
    if(slaveFd == -1) {
        perror("ptyFork: open-slave");
        exit(1);
    }

#ifdef TIOCSCTTY
    if(ioctl(slaveFd, TIOCSCTTY, 0) == -1) {
        perror("ptyFork: open-slave");
        exit(1);
    }
#endif

    if(slaveTermios != NULL) {
        struct termios t;
        memcpy(&t, slaveTermios, sizeof(struct termios));
        t.c_cc[VMIN] = 1;           // Character-at-a-time input
        t.c_cc[VKILL] = t.c_cc[VINTR];  // ctrl+C 改成擦除当前输入行
        long disable = fpathconf(STDIN_FILENO, _PC_VDISABLE);
        t.c_cc[VINTR] = disable;  // 取消中断信号
        t.c_cc[VQUIT] = disable;  // 取消退出信号
        t.c_cc[VSUSP] = disable;  // 取消暂停
        t.c_cc[VEOF] = disable;  // 取消文件结尾
        t.c_cc[VSTOP] = disable;  // 取消停止输出

        if(tcsetattr(slaveFd, TCSANOW, &t) == -1) {
            perror("ptyFork: tcsetattr");
            exit(1);
        }
    }

    if(slaveWS != NULL) {
        if(ioctl(slaveFd, TIOCSWINSZ, slaveWS) == -1) {
            perror("ptyFork: ioctl-TIOCSWINSZ");
            exit(1);
        }
    }

    /* Duplicate pty slave to be child's stdin, stdout and stderr */

    if(dup2(slaveFd, STDIN_FILENO) != STDIN_FILENO) {
        perror("ptyFork: dup2-STDIN_FILENO");
        exit(1);
    }
    if(dup2(slaveFd, STDOUT_FILENO) != STDOUT_FILENO) {
        perror("ptyFork: dup2-STDOUT_FILENO");
        exit(1);
    }
    if(dup2(slaveFd, STDERR_FILENO) != STDERR_FILENO) {
        perror("ptyFork: dup2-STDERR_FILENO");
        exit(1);
    }

    if(slaveFd > STDERR_FILENO) close(slaveFd);

    return 0;
}
