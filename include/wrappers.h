#ifndef WRAPPERS_H
#define WRAPPERS_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
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

#endif
