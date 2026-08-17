#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

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

/**
 * Like Write, but returns -1 on error instead of dying, for callers that can
 * recover. Loops on partial writes: write may legitimately return fewer bytes
 * than requested, which for small files effectively never happens, which is
 * exactly why that bug survives to production.
 */
ssize_t Writen(int fd, const void * buf, size_t count) {
    const char *p = buf;
    size_t left = count;

    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)count;
}

int Open(const char *path, int flags, mode_t mode) {
    int fd;
    while ((fd = open(path, flags, mode)) == -1 && errno == EINTR)
        ;
    return fd;
}

int Close(int fd) {
    return close(fd);                                   // never retry on EINTR: fd may already be gone
}

int Ftruncate(int fd, off_t length) {
    int r;
    while ((r = ftruncate(fd, length)) == -1 && errno == EINTR)
        ;
    return r;
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

/* ------------------------------------------- subprocess and multiplexing --- */

int Pipe(int fds[2]) {
    return pipe(fds);
}

pid_t Fork(void) {
    return fork();
}

int Dup2(int oldfd, int newfd) {
    int r;
    while ((r = dup2(oldfd, newfd)) == -1 && errno == EINTR)
        ;
    return r;
}

int Execvp(const char *file, char *const argv[]) {
    return execvp(file, argv);                          // returns only if it failed
}

/**
 * EINTR here is not an error: SIGWINCH fires on every window resize and would
 * otherwise look like a poll failure. Retrying returns to the caller with the
 * resize flag already set, so the next iteration redraws.
 */
int Poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    int r;
    while ((r = poll(fds, nfds, timeout)) == -1 && errno == EINTR)
        ;
    return r;
}

int SetNonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

pid_t Waitpid(pid_t pid, int *status, int options) {
    pid_t r;
    while ((r = waitpid(pid, status, options)) == -1 && errno == EINTR)
        ;
    return r;
}

int Kill(pid_t pid, int sig) {
    return kill(pid, sig);
}

/**
 * SIG_IGN rather than a handler: writing to a server that has already exited
 * raises SIGPIPE, whose default action terminates the editor with no message at
 * all. Ignoring it turns that into a plain EPIPE from write, which the caller
 * can see and act on.
 */
int IgnoreSignal(int signum) {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(signum, &sa, NULL);
}

long MonotonicMs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) return 0;
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
