#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>
#include <termio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include "pty_fork.h"
#include "stdbool.h"

#define BUF_SIZE    256
#define MAX_SNAME   1000

struct termios ttyOrig;

static void errExit(const char* err)
{
    printf("%s\n", err);
    exit(1);
}

static void ttyReset(void)
{
    if(tcsetattr(STDIN_FILENO, TCSANOW, &ttyOrig) == -1)
        errExit("tcsetattr");
}

/* Place terminal referred to by 'fd' in raw mode (noncanonical mode 
   with all input and output processing disabled). Return 0 on success,
   or -1 on error. If 'prevTermios' is non-NULL, then use the buffer to
   which it points to return the previous terminal settings. */
int ttySetRaw(int fd, struct termios *prevTermios)
{
    struct termios t;
    if(tcgetattr(fd, &t) == -1)
        return -1;

    if(prevTermios != NULL)
        *prevTermios = t;

    t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
                        /* Noncanonical mode, disable signals, extended
                           input processing, and echoing */

    t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR  | INLCR |
            INPCK | ISTRIP | IXON | PARMRK);
                        /* Disable special handling of CR, NL, and BREAK.
                           No 8th-bit stripping or parity error handling.
                           Disable START/STOP output flow control. */

    t.c_oflag &= ~OPOST;        /* Disable all output processing */

    t.c_cc[VMIN] = 1;           /* Character-at-a-time input */
    t.c_cc[VTIME] = 0;          /* with blocking */

    if(tcsetattr(fd, TCSAFLUSH, &t) == -1)
        return -1;

    return 0;
}

static int createTcpSer(int port)
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd == -1) {
        errExit("socket error");
    }

    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if(bind(listenfd, (const struct sockaddr*)&servaddr, sizeof(servaddr)) == -1) {
        errExit("bind error");
    }

    if(listen(listenfd, 5) == -1) {
        errExit("listen error");
    }

    printf("create tcp server on port[%d] success\n", port);
    return listenfd;
}

static void checkArgv(int argc, char *argv[])
{
    if(argc != 2) {
        printf("\t %s [cmd]\n", argv[0]);
        errExit("args error\n");
    }
}

static int g_servfd = -1;
void closeSocket()
{
    if(g_servfd != -1) close(g_servfd);
}

int main(int argc, char *argv[])
{
    checkArgv(argc, argv);

    char slaveName[MAX_SNAME];
    char* shell;
    int masterFd;
    struct winsize ws;
    fd_set inFds;
    char buf[BUF_SIZE];
    ssize_t numRead;
    pid_t childPid;

    if(tcgetattr(STDIN_FILENO, &ttyOrig) == -1)
        errExit("tcgetattr");
    if(ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0)
        errExit("ioctl-TIOCGWINSZ");

    childPid = ptyFork(&masterFd, slaveName, MAX_SNAME, &ttyOrig, &ws);
    if(childPid == -1)
        errExit("ptyFork");

    if(childPid == 0) {                 /* Child: execute a shell on pty slave */
        execlp(argv[1], argv[1], (char*)NULL);
        errExit("execlp");              /* If we get here, something went wrong */
    }

    /* Parent: relay data between terminal and pty master */

    ttySetRaw(STDIN_FILENO, &ttyOrig);
    if(atexit(ttyReset) !=0)
        errExit("atexit");
    if(atexit(closeSocket) !=0)
        errExit("atexit closeSocket");

    g_servfd = createTcpSer(5001);
    int clifd = -1;

    for(;;) {
        FD_ZERO(&inFds);
        FD_SET(STDIN_FILENO, &inFds);
        FD_SET(masterFd, &inFds);
        FD_SET(g_servfd, &inFds);
        if(clifd != -1) FD_SET(clifd, &inFds);

        int nfds = (clifd > g_servfd ? clifd : g_servfd) + 1;
        if(select(nfds, &inFds, NULL, NULL, NULL) == -1)
            errExit("select");

        if(FD_ISSET(STDIN_FILENO, &inFds)) {    /* stdin --> pty */
            numRead = read(STDIN_FILENO, buf, BUF_SIZE);
            if(numRead <= 0)
                exit(EXIT_SUCCESS);
            //printf("\nread stdin numRead = %ld\n", numRead);

            if(write(masterFd, buf, numRead) != numRead)
                printf("partial/failed write (masterFd)\n");
        }

        if(clifd != -1 && FD_ISSET(clifd, &inFds)) {    /* tcp --> pty */
            numRead = read(clifd, buf, BUF_SIZE);
            if(numRead <= 0) {
                printf("read clifd num[%ld] <= 0\n", numRead);
                close(clifd);
                clifd = -1;
            }

            if(write(masterFd, buf, numRead) != numRead)
                printf("partial/failed write (masterFd)\n");
        }

        if(FD_ISSET(g_servfd, &inFds)) {
            if(clifd != -1) {
                FD_CLR(clifd, &inFds);
                close(clifd);
            }
            clifd = accept(g_servfd, NULL, NULL);
            if(clifd == -1) {
                printf("accept error\n");
            }

            char* name = ptySlaveName();
            printf("accept clifd = %d, slave name = %s\n", clifd, name);
            numRead = read(clifd, buf, BUF_SIZE);
            if(numRead == sizeof(struct winsize)) {
                int slaveFd = open(name, O_RDWR);
                if(slaveFd == -1) {
                    perror("ptyFork: open-slave");
                }

                struct winsize *ws = (struct winsize*)buf;
                if(ioctl(slaveFd, TIOCSWINSZ, ws) == -1) {
                    perror("ptyFork: ioctl-TIOCSWINSZ");
                }
                close(slaveFd);
            }
        }

        if(FD_ISSET(masterFd, &inFds)) {        /* pty --> stdout+file */
            numRead = read(masterFd, buf, BUF_SIZE);
            if(numRead <= 0) {
                printf("read masterFd num[%ld] <= 0\n", numRead);
                if(g_servfd != -1) close(g_servfd);
                exit(EXIT_SUCCESS);
            }

            if(write(STDOUT_FILENO, buf, numRead) != numRead)
                printf("partial/failed write (STDOUT_FILENO)\n");
            if(clifd != -1 && write(clifd, buf, numRead) != numRead)
                printf("partial/failed write (clifd)\n");
        }
    }
}
