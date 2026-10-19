/* POSIX implementation of runtime_log.h. */
#define _GNU_SOURCE
#include "runtime_log.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int echo_source = -1, echo_target = -1, log_fd = -1;
static atomic_int echo_busy;

static void make_directories(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    }
    mkdir(path, 0777);
}

static unsigned last_number(const char *dir) {
    DIR *d = opendir(dir);
    unsigned last = 0;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));) {
        unsigned number = 0;
        int used = 0;
        if (sscanf(e->d_name, "dragons_crown_pro_%u.log%n", &number, &used) == 1 && used > 0 && !e->d_name[used] && number > last)
            last = number;
    }
    closedir(d);
    return last;
}

int runtime_log_create(const char *dir, const char *file, char *path, size_t size, void **handle) {
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    if (file) {
        if (snprintf(path, size, "%s", file) >= (int)size) return 0;
        char *separator = strrchr(path, '/');
        if (separator && separator > path) {
            *separator = 0;
            make_directories(path);
            *separator = '/';
        }
        int fd = open(path, flags | O_TRUNC, 0666);
        if (fd < 0) return 0;
        *handle = (void *)(intptr_t)fd;
        return 1;
    }
    make_directories(dir);
    for (unsigned number = last_number(dir) + 1, tries = 0; tries < 1000; ++number, ++tries) {
        if (snprintf(path, size, "%s/dragons_crown_pro_%03u.log", dir, number) >= (int)size) return 0;
        int fd = open(path, flags | O_EXCL, 0666);
        if (fd >= 0) {
            *handle = (void *)(intptr_t)fd;
            return 1;
        }
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static void write_all(int fd, const char *data, ssize_t size) {
    while (size > 0) {
        ssize_t n = write(fd, data, (size_t)size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        data += n;
        size -= n;
    }
}

static void *echo_log(void *unused) {
    (void)unused;
    char buffer[4096];
    struct pollfd wait = {echo_source, POLLIN, 0};
    for (;;) {
        if (poll(&wait, 1, -1) < 0 && errno != EINTR) return NULL;
        atomic_store(&echo_busy, 1);
        ssize_t n = read(echo_source, buffer, sizeof(buffer));
        if (n > 0) {
            write_all(log_fd, buffer, n);
            if (echo_target >= 0) write_all(echo_target, buffer, n);
        }
        atomic_store(&echo_busy, 0);
        if (n == 0) return NULL;
    }
}

int runtime_log_start(const char *dir, const char *file, char *path, size_t size) {
    void *created;
    if (!runtime_log_create(dir, file, path, size, &created)) return 0;
    runtime_log_set_session_path(path);
    log_fd = (int)(intptr_t)created;
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC)) {
        close(log_fd);
        log_fd = -1;
        return 0;
    }
    fflush(NULL);
    echo_target = dup(1);
    const int saved_error = dup(2);
    echo_source = pipes[0];
    pthread_t thread;
    const int redirected = dup2(pipes[1], 1) >= 0 && dup2(pipes[1], 2) >= 0 &&
                           pthread_create(&thread, NULL, echo_log, NULL) == 0;
    close(pipes[1]);
    if (!redirected) {
        /* Nothing would read the pipe: put the console back. */
        if (echo_target >= 0) dup2(echo_target, 1);
        if (saved_error >= 0) dup2(saved_error, 2);
        close(pipes[0]);
        close(log_fd);
        echo_source = log_fd = -1;
    } else {
        pthread_detach(thread);
    }
    if (saved_error >= 0) close(saved_error);
    return redirected;
}

void runtime_log_flush(void) {
    fflush(NULL);
    if (echo_source < 0) return;
    for (int wait = 0; wait < 400; ++wait) {
        int pending = 0;
        if (!ioctl(echo_source, FIONREAD, &pending) && !pending && !atomic_load(&echo_busy)) return;
        usleep(5000);
    }
}
