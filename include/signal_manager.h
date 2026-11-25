#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <time.h>

/*
 * signal_manager - installs the watchdog's signal handlers and turns
 * asynchronous signals into flags that the main loop polls.
 *
 * Design (async-signal-safety):
 *   - Every handler only stores into a `volatile sig_atomic_t` flag.
 *     No logging, no malloc, no fork, no stdio inside a handler.
 *   - sm_install() BLOCKS all handled signals. They are only unblocked
 *     atomically inside sm_wait() / sm_wait_timeout() (sigsuspend / pselect),
 *     so a signal can never arrive between "check flags" and "go to sleep",
 *     and the sm_take_*() read-and-clear functions are race-free.
 *
 * Handled signals:
 *   SIGINT, SIGTERM -> shutdown request (a second one forces SIGKILL)
 *   SIGUSR1         -> status report request
 *   SIGCHLD         -> a child changed state, reap it with waitpid()
 *   SIGALRM         -> periodic check tick (timer armed by process_manager)
 */

/* Installs all handlers with sigaction() and blocks the handled signals.
 * Must be called before the first fork(). Returns 0 on success, -1 on error. */
int sm_install(void);

/* Call in the CHILD between fork() and exec(): restores default
 * dispositions and the original signal mask. Without this the worker
 * would inherit a blocked SIGTERM across exec() and never see it.
 * Uses only async-signal-safe calls. */
void sm_child_reset(void);

/* Sleeps until at least one handled signal has been delivered. */
void sm_wait(void);

/* Like sm_wait() but returns after at most *timeout.
 * Returns 1 if woken by a signal, 0 on timeout, -1 on error. */
int sm_wait_timeout(const struct timespec *timeout);

/* Returns the signal number (SIGINT/SIGTERM) that requested shutdown,
 * or 0 if none. Not cleared: once requested, shutdown stays requested. */
int sm_shutdown_requested(void);

/* Non-zero if a second SIGINT/SIGTERM arrived after the first one. */
int sm_force_requested(void);

/* Read-and-clear accessors: return non-zero if the event happened
 * since the last call. */
int sm_take_status_request(void); /* SIGUSR1 */
int sm_take_child_event(void);    /* SIGCHLD */
int sm_take_alarm(void);          /* SIGALRM */

/* "SIGINT", "SIGTERM", ... for log messages; "signal" if unknown. */
const char *sm_signal_name(int signo);

#endif
