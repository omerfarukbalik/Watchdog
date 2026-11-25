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
static int     shutting_down = 0; /* set by pm_begin_shutdown(): no restarts */

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

/* ------------------------------------------------- reap and restart */

/* Finds the RUNNING child with this pid. Only running children can still
 * be reaped; dead ones keep their last pid, which the system may have
 * reused for a new process, so they must not match. */
static child_t *find_running(pid_t pid)
{
    for (int i = 0; i < n_children; i++)
        if (children[i].state == CS_RUNNING && children[i].pid == pid)
            return &children[i];
    return NULL;
}

/* After a crash: restart the child if it has restarts left, else give up. */
static void restart_or_give_up(child_t *c)
{
    if (c->restarts >= MAX_RESTARTS) {
        c->state = CS_GAVE_UP;
        log_event("GIVE UP %s after %d restarts", c->name, c->restarts);
        return;
    }

    c->restarts++;
    log_event("RESTART %s attempt %d/%d", c->name, c->restarts, MAX_RESTARTS);
    if (start_child(c) == -1) {
        /* fork() failed (e.g. EAGAIN): the periodic check retries it. */
        c->state = CS_PENDING;
        log_event("PENDING %s: will retry at the next check", c->name);
    }
}

/* Decides what a finished child's wait status means and acts on it. */
static void handle_exit(child_t *c, int status)
{
    if (shutting_down) {
        /* We asked it to stop (or it ended on its own meanwhile): never
         * restart during shutdown, just record how it ended. */
        c->state = CS_STOPPED;
        if (WIFSIGNALED(status))
            log_event("STOP %s pid=%d signal %d", c->name, (int)c->pid, WTERMSIG(status));
        else
            log_event("STOP %s pid=%d exit code=%d", c->name, (int)c->pid,
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        /* Normal termination: the child finished its job. */
        c->state = CS_EXITED;
        log_event("EXIT %s pid=%d normal", c->name, (int)c->pid);
        return;
    }

    /* Unexpected termination. */
    if (WIFEXITED(status))
        log_event("CRASH %s pid=%d exit code=%d",
                  c->name, (int)c->pid, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        log_event("CRASH %s pid=%d signal %d (%s)",
                  c->name, (int)c->pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        log_event("CRASH %s pid=%d status=0x%x", c->name, (int)c->pid, (unsigned)status);

    restart_or_give_up(c);
}

void pm_reap(void)
{
    /* One SIGCHLD can stand for several children (signals do not queue),
     * so reap until waitpid() says nothing more has finished. */
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0)
            return; /* children exist, none has finished */
        if (pid == -1) {
            if (errno == EINTR)
                continue;
            if (errno != ECHILD) /* ECHILD: no children at all */
                log_event("REAP waitpid failed: %s", strerror(errno));
            return;
        }

        child_t *c = find_running(pid);
        if (c == NULL) {
            log_event("REAP unknown child pid=%d", (int)pid);
            continue;
        }
        handle_exit(c, status);
    }
}

/* ------------------------------------------------------ periodic check */

int pm_start_timer(unsigned seconds)
{
    if (seconds == 0) {
        log_event("TIMER not started: interval must be > 0");
        return -1;
    }

    /* it_value = first expiry, it_interval = period after that. */
    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_value.tv_sec = (time_t)seconds;
    it.it_interval.tv_sec = (time_t)seconds;

    if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
        log_event("TIMER setitimer failed: %s", strerror(errno));
        return -1;
    }
    log_event("TIMER periodic check every %us", seconds);
    return 0;
}

void pm_stop_timer(void)
{
    struct itimerval it;
    memset(&it, 0, sizeof it); /* all zero = disarm */
    if (setitimer(ITIMER_REAL, &it, NULL) == -1)
        log_event("TIMER disarm failed: %s", strerror(errno));
}

void pm_check(void)
{
    /* Safety net: reap anything whose SIGCHLD was missed. */
    pm_reap();

    /* Retry children whose (re)start failed because fork() failed. */
    if (!shutting_down) {
        for (int i = 0; i < n_children; i++)
            if (children[i].state == CS_PENDING)
                (void)start_child(&children[i]); /* logs START or the error */
    }

    int count[CS_STOPPED + 1] = { 0 };
    for (int i = 0; i < n_children; i++)
        count[children[i].state]++;

    log_event("CHECK running=%d pending=%d exited=%d gave_up=%d stopped=%d",
              count[CS_RUNNING], count[CS_PENDING], count[CS_EXITED],
              count[CS_GAVE_UP], count[CS_STOPPED]);
}

/* ------------------------------------------------------------ shutdown */

void pm_begin_shutdown(void)
{
    if (shutting_down)
        return;
    shutting_down = 1;

    /* A child waiting for a restart retry will not be started any more. */
    for (int i = 0; i < n_children; i++) {
        if (children[i].state == CS_PENDING) {
            children[i].state = CS_STOPPED;
            log_event("STOP %s: pending start cancelled", children[i].name);
        }
    }
}

int pm_signal_all(int sig)
{
    int sent = 0;
    for (int i = 0; i < n_children; i++) {
        child_t *c = &children[i];
        if (c->state != CS_RUNNING)
            continue;
        /* A RUNNING child has not been reaped yet, so its pid cannot have
         * been reused: kill() reaches the right process. */
        if (kill(c->pid, sig) == -1)
            log_event("SIGNAL %s pid=%d failed: %s", c->name, (int)c->pid, strerror(errno));
        else
            sent++;
    }
    return sent;
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
