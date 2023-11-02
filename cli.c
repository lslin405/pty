#include <stdio.h>
#include <termio.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#define BUF_SIZE    256
struct termios ttyOrig;

static void errExit(const char* err)
{
    printf("%s error\n", err);
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

static int createTcpCli(int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd == -1) {
        errExit("socket error");
    }

    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);

    if(connect(sockfd, (const struct sockaddr*)&servaddr, sizeof(servaddr)) == -1) {
        errExit("connect");
    }

    printf("connect to server[p:%d] success\n", port);
    return sockfd;
}

bool isControlKey(char* buf, ssize_t *len)
{
    if(*len == 1) {
        // tab键
        if(buf[0] == 9) return true;
        // ctrl+C
        if(buf[0] == 3) return true;

        // ctrl+D
        if(buf[0] == 4) {
            buf[0] = 13;    // 转成回车
            return true;
        }
    }

    // 方向键
    if(*len == 3 && buf[0] == 27 && buf[1] == 91) {
        if(buf[2] == 65 || buf[2] == 66 || buf[2] == 67 || buf[2] == 68) {
            return true;
        }
    }

    return false;
}

bool isEnter(char* buf, ssize_t *len)
{
    // 回车键
    if(*len == 1 && buf[0] == 13) {
        return true;
    }

    return false;
}

char* cmdBuf(char* buf, ssize_t *len)
{
    static char cmd[BUF_SIZE];
    static int index = 0;

    if(isControlKey(buf, len)) {
        index = 0;
        return NULL;
    }

    if(isEnter(buf, len)) {
        cmd[index] = 0;
        index = 0;

        // 退出命令取消执行
        if(strncmp(cmd, "exit", 4) == 0) {
            buf[0] = 3;
        }

        return cmd;
    }

    memcpy(cmd + index, buf, *len);
    index += *len;

    return NULL;
}

static int g_sockfd = -1;
void closeSocket()
{
    if(g_sockfd != -1) close(g_sockfd);
}

void main()
{
    ttySetRaw(STDIN_FILENO, &ttyOrig);
    if(atexit(ttyReset) !=0)
        errExit("atexit ttyReset");
    if(atexit(closeSocket) !=0)
        errExit("atexit closeSocket");

    struct winsize ws;
    if(ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0)
        errExit("ioctl-TIOCGWINSZ");

    fd_set inFds;
    char buf[BUF_SIZE];
    ssize_t numRead;
    g_sockfd = createTcpCli(5001);

    // 改变终端大小
    if(write(g_sockfd, &ws, sizeof(struct winsize)) != sizeof(struct winsize))
        printf("change winsize write (g_sockfd)\n");

    for(;;) {
        FD_ZERO(&inFds);
        FD_SET(STDIN_FILENO, &inFds);
        FD_SET(g_sockfd, &inFds);

        int nfds = g_sockfd > STDIN_FILENO ? g_sockfd : STDIN_FILENO;
        if(select(nfds + 1, &inFds, NULL, NULL, NULL) == -1) {
            
            printf("error: %s", strerror(errno));
            errExit("select");
        }

        if(FD_ISSET(STDIN_FILENO, &inFds)) {    /* stdin --> tcp */
            numRead = read(STDIN_FILENO, buf, BUF_SIZE);
            if(numRead <= 0)
                exit(EXIT_SUCCESS);

            /**************************************/
            /*
            printf("input num = %ld\n", numRead);
            for(int i = 0; i < numRead; i++){
                printf("buf[%d] = %d\n", i, buf[i]);
            }
            */

            char* cmd = cmdBuf(buf, &numRead);

            if(write(g_sockfd, buf, numRead) != numRead)
                printf("partial/failed write (g_sockfd)\n");

            if(cmd != NULL && strcmp(cmd, "exit") == 0) {
                exit(EXIT_SUCCESS);
            }
        }

        if(FD_ISSET(g_sockfd, &inFds)) {    /* tcp --> stdout */
            numRead = read(g_sockfd, buf, BUF_SIZE);
            if(numRead <= 0)
                exit(EXIT_SUCCESS);

            if(write(STDOUT_FILENO, buf, numRead) != numRead)
                printf("partial/failed write (STDOUT_FILENO)\n");
        }

    }
}
