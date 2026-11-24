/*
 * signal_manager.c - sigaction() handlers that only set flags.
 *
 * The handlers below are the ONLY code in the watchdog that runs in
 * signal context. Each one stores into a `volatile sig_atomic_t` and
 * returns; all real work (waitpid, logging, kill, fork) is done by the
 * main loop after sm_wait() returns.
 */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

#include "signal_manager.h"

static volatile sig_atomic_t g_shutdown_sig = 0; /* SIGINT/SIGTERM number */
static volatile sig_atomic_t g_force = 0;        /* second shutdown signal */
static volatile sig_atomic_t g_status = 0;       /* SIGUSR1 */
static volatile sig_atomic_t g_child = 0;        /* SIGCHLD */
static volatile sig_atomic_t g_alarm = 0;        /* SIGALRM */

static const int handled[] = { SIGINT, SIGTERM, SIGUSR1, SIGCHLD, SIGALRM };
#define N_HANDLED (sizeof handled / sizeof handled[0])

static sigset_t g_orig_mask; /* mask the process started with */
static sigset_t g_wait_mask; /* g_orig_mask minus the handled signals */

/* ------------------------------------------------------------ handlers */

static void on_shutdown(int signo)
{
    /* sa_mask blocks SIGINT and SIGTERM while this runs, so the
     * read-then-write below cannot be interrupted by another one. */
    if (g_shutdown_sig != 0)
        g_force = 1;
    else
        g_shutdown_sig = signo;
}

static void on_usr1(int signo)
{
    (void)signo;
    g_status = 1;
}

static void on_chld(int signo)
{
    (void)signo;
    g_child = 1; /* several SIGCHLDs may merge into one: the main loop
                  * reaps with waitpid(-1, ..., WNOHANG) in a loop */
}

static void on_alrm(int signo)
{
    (void)signo;
    g_alarm = 1;
}

/* ------------------------------------------------------------- install */

static int install_one(int signo, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;

    /* Block every handled signal while any handler runs, so handlers
     * never interrupt each other. */
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < N_HANDLED; i++)
        sigaddset(&sa.sa_mask, handled[i]);

    if (sigaction(signo, &sa, NULL) == -1) {
        fprintf(stderr, "signal_manager: sigaction(%s): %s\n",
                sm_signal_name(signo), strerror(errno));
        return -1;
    }
    return 0;
}

int sm_install(void)
{
    sigset_t block;
    sigemptyset(&block);
    for (size_t i = 0; i < N_HANDLED; i++)
        sigaddset(&block, handled[i]);

    /* Block first, then install: a signal arriving in between stays
     * pending and is delivered at the first sm_wait(). */
    if (sigprocmask(SIG_BLOCK, &block, &g_orig_mask) == -1) {
        perror("signal_manager: sigprocmask");
        return -1;
    }

    g_wait_mask = g_orig_mask;
    for (size_t i = 0; i < N_HANDLED; i++)
        sigdelset(&g_wait_mask, handled[i]);

    /* SA_RESTART: a system call interrupted by a handler is restarted
     *   instead of failing with EINTR (defence in depth; signals are
     *   normally only unblocked inside sm_wait()).
     * SA_NOCLDSTOP: no SIGCHLD when a child is merely stopped/continued.
     * Note: SIGINT is installed even if it was SIG_IGN at startup
     *   (background jobs of a script), so `kill -INT` always works. */
    if (install_one(SIGINT,  on_shutdown, SA_RESTART) == -1 ||
        install_one(SIGTERM, on_shutdown, SA_RESTART) == -1 ||
        install_one(SIGUSR1, on_usr1,     SA_RESTART) == -1 ||
        install_one(SIGCHLD, on_chld,     SA_RESTART | SA_NOCLDSTOP) == -1 ||
        install_one(SIGALRM, on_alrm,     SA_RESTART) == -1)
        return -1;

    return 0;
}

void sm_child_reset(void)
{
    /* Runs between fork() and exec(): async-signal-safe calls only. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < N_HANDLED; i++)
        (void)sigaction(handled[i], &sa, NULL);

    (void)sigprocmask(SIG_SETMASK, &g_orig_mask, NULL);
}

/* ---------------------------------------------------------------- wait */

void sm_wait(void)
{
    /* Atomically unblocks the handled signals and sleeps; always returns
     * -1/EINTR after a handler ran, with the old (blocking) mask back. */
    (void)sigsuspend(&g_wait_mask);
}

int sm_wait_timeout(const struct timespec *timeout)
{
    /* pselect() with no descriptors = sigsuspend() with a timeout. */
    int r = pselect(0, NULL, NULL, NULL, timeout, &g_wait_mask);
    if (r == -1)
        return errno == EINTR ? 1 : -1;
    return 0;
}

/* ------------------------------------------------------------- getters */

/* All handled signals are blocked outside sm_wait(), so these
 * read-and-clear sequences cannot race with a handler. */

int sm_shutdown_requested(void)
{
    return (int)g_shutdown_sig;
}

int sm_force_requested(void)
{
    return (int)g_force;
}

int sm_take_status_request(void)
{
    int v = (int)g_status;
    g_status = 0;
    return v;
}

int sm_take_child_event(void)
{
    int v = (int)g_child;
    g_child = 0;
    return v;
}

int sm_take_alarm(void)
{
    int v = (int)g_alarm;
    g_alarm = 0;
    return v;
}

const char *sm_signal_name(int signo)
{
    switch (signo) {
    case SIGINT:  return "SIGINT";
    case SIGTERM: return "SIGTERM";
    case SIGUSR1: return "SIGUSR1";
    case SIGCHLD: return "SIGCHLD";
    case SIGALRM: return "SIGALRM";
    case SIGKILL: return "SIGKILL";
    default:      return "signal";
    }
}
