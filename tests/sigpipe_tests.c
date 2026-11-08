/* Focused test of the loader's SIGPIPE handling (POSIX only): a write to a socket whose peer has
 * closed its end must fail with EPIPE, not kill the process. Local AF_UNIX socketpair, no network. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

/* Runs in a forked child: closes the read end of a socketpair (as a peer closing its connection
 * would), writes to the write end, and reports how the write and the process behaved. */
static int write_to_closed_peer(int ignore_sigpipe) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) _exit(90);
    if (ignore_sigpipe) {
        struct sigaction ignore_pipe = {0};
        ignore_pipe.sa_handler = SIG_IGN;
        sigemptyset(&ignore_pipe.sa_mask);
        sigaction(SIGPIPE, &ignore_pipe, NULL);
    }
    close(fds[0]); /* the peer's end */
    char byte = 'x';
    /* The first write after the peer closes often still succeeds (buffered by the kernel); SIGPIPE
     * fires once the peer's close has been observed, which a short retry loop waits out. */
    ssize_t sent = -1;
    int saved_errno = 0;
    for (int attempt = 0; attempt < 200; ++attempt) {
        sent = write(fds[1], &byte, 1);
        saved_errno = errno;
        if (sent < 0 && saved_errno == EPIPE) break;
        usleep(1000);
    }
    close(fds[1]);
    _exit(sent < 0 && saved_errno == EPIPE ? 0 : 91);
}

static void run_child(int ignore_sigpipe, int *exited_normally, int *exit_code, int *killed_by_sigpipe) {
    pid_t child = fork();
    if (child == 0) write_to_closed_peer(ignore_sigpipe);
    int status = 0;
    waitpid(child, &status, 0);
    *exited_normally = WIFEXITED(status);
    *exit_code = *exited_normally ? WEXITSTATUS(status) : -1;
    *killed_by_sigpipe = WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE;
}

/* Without the fix, a write to a socket whose peer closed the connection kills the process; this
 * establishes the test is meaningful, not vacuously passing. */
static void test_default_disposition_is_killed_by_sigpipe(void) {
    int exited_normally = 0, exit_code = 0, killed_by_sigpipe = 0;
    run_child(0, &exited_normally, &exit_code, &killed_by_sigpipe);
    CHECK(killed_by_sigpipe);
    CHECK(!exited_normally);
}

/* With SIGPIPE ignored, as the loader now does at startup, the same write fails the caller with
 * EPIPE and the process survives. */
static void test_ignored_sigpipe_survives_the_write(void) {
    int exited_normally = 0, exit_code = 0, killed_by_sigpipe = 0;
    run_child(1, &exited_normally, &exit_code, &killed_by_sigpipe);
    CHECK(!killed_by_sigpipe);
    CHECK(exited_normally && exit_code == 0);
}

int main(void) {
    test_default_disposition_is_killed_by_sigpipe();
    test_ignored_sigpipe_survives_the_write();
    printf("sigpipe tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
