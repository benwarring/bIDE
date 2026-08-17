#ifndef WRAPPERS_H
#define WRAPPERS_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>

int Read(int fd, void * buf, size_t count);

int Write(int fd, const void * buf, size_t count);

/* The four below report failure to the caller instead of dying, because saving
 * a file has a real recovery path: warn the user and keep the dirty counter
 * set. Same precedent as Ioctl below. */

int Open(const char *path, int flags, mode_t mode);

int Close(int fd);

int Ftruncate(int fd, off_t length);

ssize_t Writen(int fd, const void * buf, size_t count);

void Tcgetattr(int fd, struct termios *termios_p);

void Tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

int Ioctl(int fd, unsigned long request, void *arg);

void Sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

FILE *Fopen(const char *path, const char *mode);

void Fclose(FILE *stream);

void *Malloc(size_t size);

void *Realloc(void *ptr, size_t size);

/* ---------------------------------------------------------------------------
 * Subprocess and multiplexing, for the language server in Milestone 5.
 *
 * Every one of these reports failure to the caller rather than dying. A
 * language server that will not start, or that dies mid-session, must leave a
 * working text editor behind — the same reasoning that makes Open and Writen
 * recoverable, applied to a subsystem the editor is designed to work without.
 * ------------------------------------------------------------------------- */

int Pipe(int fds[2]);

pid_t Fork(void);

int Dup2(int oldfd, int newfd);

/** Only returns on failure; on success the process image is gone. */
int Execvp(const char *file, char *const argv[]);

/**
 * Retries on EINTR, which SIGWINCH causes on every window resize. Returns the
 * number of ready descriptors, or -1 for a real error.
 */
int Poll(struct pollfd *fds, nfds_t nfds, int timeout);

/** Adds O_NONBLOCK, so a partial message never stalls the editor mid-read. */
int SetNonblock(int fd);

pid_t Waitpid(pid_t pid, int *status, int options);

int Kill(pid_t pid, int sig);

/** Sets a signal to be ignored; used for SIGPIPE. */
int IgnoreSignal(int signum);

/**
 * Milliseconds from a monotonic clock. Monotonic rather than wall time because
 * these drive timeouts, and a clock adjustment mid-session must not make a
 * debounce wait an hour or fire instantly.
 */
long MonotonicMs(void);

#endif
