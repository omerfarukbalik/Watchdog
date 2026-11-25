/*
 * process_manager.c - child process management for the watchdog.
 *
 *   - starts every child with fork() + execv(), each in its own process group
 *   - reaps finished children with waitpid() after SIGCHLD
 *   - tells a normal exit (code 0) apart from a crash (other code / signal)
 *   - restarts crashed children, at most MAX_RESTARTS times each
 *   - arms the periodic check timer with setitimer() (SIGALRM)
 *
 * Every function here is called from the main loop, never from a signal
 * handler, so logging is allowed. The only restricted code is the child
 * side of fork() (exec_child): from fork() to execv() it uses
 * async-signal-safe calls only (no log_event, no printf, no malloc).
 */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

/* Static table: no malloc, so nothing to free and nothing to leak. */
static child_t children[PM_MAX_CHILDREN];
static int     n_children = 0;

/* ------------------------------------------------- child side of fork() */

/* write() a whole string to stderr. Async-signal-safe. */
static void safe_write(const char *s)
{
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t w = write(STDERR_FILENO, s, len);
        if (w == -1 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        s += w;
        len -= (size_t)w;
    }
}

/* write() a non-negative int in decimal. Async-signal-safe. */
static void safe_write_int(int v)
{
    char buf[16];
    int i = (int)sizeof buf - 1;
    buf[i] = '\0';
    if (v < 0)
        v = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && i > 0);
    safe_write(&buf[i]);
}

/* Runs in the child right after fork() and never returns. */
static _Noreturn void exec_child(char *const *argv)
{
    /* Own process group: a Ctrl+C in the terminal is sent to the
     * watchdog's group only, so the watchdog alone decides how its
     * children are stopped. */
    (void)setpgid(0, 0);

    /* Default signal dispositions and the original signal mask.
     * Without this the worker would inherit a blocked SIGTERM. */
    sm_child_reset();

    execv(argv[0], argv);

    /* Only reached if execv() failed. strerror() is not async-signal-safe,
     * so print the errno number. The parent sees exit code 127. */
    int err = errno;
    safe_write("watchdog: execv(");
    safe_write(argv[0]);
    safe_write(") failed, errno ");
    safe_write_int(err);
    safe_write("\n");
    _exit(127);
}

/* ------------------------------------------------------------- start */

/* Starts (or restarts) one child. Returns 0 on success, -1 if fork failed. */
static int start_child(child_t *c)
{
    pid_t pid = fork();
    if (pid == -1) {
        log_event("START %s failed: fork: %s", c->name, strerror(errno));
        return -1;
    }
    if (pid == 0)
        exec_child(c->argv); /* child: never returns */

    /* Parent: set the child's group too. Parent and child race; doing it
     * on both sides means the group is set before either one goes on.
     * EACCES (child already called execv) and ESRCH (child already gone)
     * just mean the child got there first. */
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH)
        log_event("START %s warning: setpgid: %s", c->name, strerror(errno));

    c->pid = pid;
    c->state = CS_RUNNING;
    c->started_at = time(NULL);
    log_event("START %s pid=%d", c->name, (int)pid);
    return 0;
}

int pm_init(const child_spec_t *specs, int n)
{
    if (specs == NULL || n < 1 || n > PM_MAX_CHILDREN) {
        log_event("PM init failed: need 1..%d children, got %d", PM_MAX_CHILDREN, n);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (specs[i].name == NULL || specs[i].argv == NULL || specs[i].argv[0] == NULL) {
            log_event("PM init failed: child %d has no name or no argv[0]", i);
            return -1;
        }
    }

    memset(children, 0, sizeof children);
    for (int i = 0; i < n; i++) {
        children[i].name = specs[i].name;
        children[i].argv = specs[i].argv;
        children[i].pid = 0;              /* not started yet */
        children[i].state = CS_PENDING;   /* start is due    */
        children[i].restarts = 0;
        children[i].started_at = 0;
    }
    n_children = n;
    return 0;
}

int pm_start_all(void)
{
    if (n_children == 0) {
        log_event("PM start failed: pm_init() was not called");
        return -1;
    }

    int failed = 0;
    for (int i = 0; i < n_children; i++)
        if (start_child(&children[i]) == -1)
            failed++;

    if (failed > 0) {
        log_event("PM %d of %d child(ren) could not be started", failed, n_children);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------- accessors */

int pm_running_count(void)
{
    int running = 0;
    for (int i = 0; i < n_children; i++)
        if (children[i].state == CS_RUNNING)
            running++;
    return running;
}

const child_t *pm_children(int *n)
{
    if (n != NULL)
        *n = n_children;
    return children;
}

const char *pm_state_name(child_state_t s)
{
    switch (s) {
    case CS_RUNNING: return "RUNNING";
    case CS_PENDING: return "PENDING";
    case CS_EXITED:  return "EXITED";
    case CS_GAVE_UP: return "GAVE_UP";
    case CS_STOPPED: return "STOPPED";
    }
    return "UNKNOWN";
}
