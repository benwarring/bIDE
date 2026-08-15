#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "terminal.h"
#include "wrappers.h"

/**
 * A simple wrapper for the standard read function
 */
int Read(int fd, void * buf, size_t count) {
    ssize_t r;
    while (1) {
        r = read(fd, buf, count);
        if (r != -1) {
            return r;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN) {
            return 0;                                   // no input ready, same as VTIME expiry
        }
        die("read");
    }
}

/**
 * A simple wrapper for the standard write function, looping until every byte is out
 */
int Write(int fd, const void * buf, size_t count) {
    const char *p = buf;
    size_t left = count;

    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w == -1) {
            if (errno == EINTR) continue;
            die("write");
        }
        left -= (size_t)w;
        p += w;
    }
    return (int)count;
}

void Tcgetattr(int fd, struct termios *termios_p) {
    if (tcgetattr(fd, termios_p) == -1) die("tcgetattr");
}

void Tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    if (tcsetattr(fd, optional_actions, termios_p) == -1) die("tcsetattr");
}

/**
 * Retries on EINTR but returns failure to the caller, since some ioctls
 * (TIOCGWINSZ in particular) have a legitimate fallback path
 */
int Ioctl(int fd, unsigned long request, void *arg) {
    int r;
    while ((r = ioctl(fd, request, arg)) == -1 && errno == EINTR)
        ;
    return r;
}

void Sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (sigaction(signum, act, oldact) == -1) die("sigaction");
}

FILE *Fopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (f == NULL) die("fopen");
    return f;
}

void Fclose(FILE *stream) {
    if (fclose(stream) == EOF) die("fclose");
}

void *Malloc(size_t size) {
    void *p = malloc(size);
    if (p == NULL) die("malloc");
    return p;
}

void *Realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == NULL) die("realloc");
    return p;
}
