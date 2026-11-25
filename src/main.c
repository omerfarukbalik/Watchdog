/*
 * main.c - watchdog entry point: main control loop and graceful shutdown.
 *
 * Flow:
 *   1. open the log, install signal handlers (all handled signals blocked)
 *   2. start the children and the periodic check timer
 *   3. main loop: sleep in sm_wait() until a signal arrives, then act on
 *      the flags the handlers set (reap / check / status report)
 *   4. on SIGINT or SIGTERM: stop restarting, SIGTERM every child, wait
 *      up to SHUTDOWN_TIMEOUT seconds, SIGKILL whatever is left, reap all
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#define CHECK_INTERVAL   5 /* seconds between SIGALRM periodic checks */
#define SHUTDOWN_TIMEOUT 5 /* seconds children get to exit after SIGTERM */
#define KILL_REAP_LIMIT  3 /* seconds to reap children after SIGKILL */

/* Supervised children. Run the watchdog from the project root. */
static char *const alpha_argv[] = { "./worker", "alpha", "forever",    NULL };
static char *const beta_argv[]  = { "./worker", "beta",  "exit-ok", "8", NULL };
static char *const gamma_argv[] = { "./worker", "gamma", "crash",   "4", NULL };

static const child_spec_t children[] = {
    { "alpha", alpha_argv },
    { "beta",  beta_argv  },
    { "gamma", gamma_argv },
};
#define N_CHILDREN ((int)(sizeof children / sizeof children[0]))

/* ------------------------------------------------------------- helpers */

static double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static struct timespec to_timespec(double seconds)
{
    struct timespec ts;
    if (seconds < 0)
        seconds = 0;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    return ts;
}

/* Handles the events that matter both while running and while shutting
 * down. Reaping comes first so a status report shows up-to-date states. */
static void handle_common_events(void)
{
    if (sm_take_child_event())
        pm_reap();
    if (sm_take_status_request())
        log_status_report();
}

/* Waits until no child is running or `limit` seconds have passed,
 * still reaping and answering SIGUSR1 meanwhile. Returns early (with
 * children still running) if `stop_on_force` and a second shutdown
 * signal arrives. */
static void wait_for_children(double limit, int stop_on_force)
{
    double deadline = now_monotonic() + limit;

    while (pm_running_count() > 0) {
        if (stop_on_force && sm_force_requested())
            return;

        double left = deadline - now_monotonic();
        if (left <= 0)
            return;

        struct timespec ts = to_timespec(left);
        if (sm_wait_timeout(&ts) == -1) {
            log_event("SHUTDOWN wait failed: %s", strerror(errno));
            return;
        }
        (void)sm_take_alarm(); /* periodic checks are over */
        handle_common_events();
    }
}

/* ------------------------------------------------------------ shutdown */

static void graceful_shutdown(void)
{
    int sig = sm_shutdown_requested();
    if (sig != 0)
        log_event("SHUTDOWN requested by %s", sm_signal_name(sig));
    else
        log_event("SHUTDOWN requested after startup failure");

    /* 1. Stop launching new processes: no more restarts, no more checks. */
    pm_begin_shutdown();
    pm_stop_timer();
    (void)sm_take_alarm();

    /* 2. Children that died just before the request are reaped now so
     *    they are not signalled (their PID could already be reused). */
    handle_common_events();

    /* 3. Ask every running child to terminate. */
    int n = pm_signal_all(SIGTERM);
    if (n > 0) {
        log_event("SHUTDOWN sent SIGTERM to %d child(ren), waiting up to %ds",
                  n, SHUTDOWN_TIMEOUT);
        wait_for_children(SHUTDOWN_TIMEOUT, 1);
    }

    /* 4. Controlled SIGKILL for children that did not exit in time
     *    (or immediately, if the user pressed Ctrl+C twice). */
    int left = pm_running_count();
    if (left > 0) {
        if (sm_force_requested())
            log_event("SHUTDOWN forced by second signal: sending SIGKILL to %d child(ren)",
                      left);
        else
            log_event("SHUTDOWN timeout after %ds: sending SIGKILL to %d child(ren)",
                      SHUTDOWN_TIMEOUT, left);
        (void)pm_signal_all(SIGKILL);
        wait_for_children(KILL_REAP_LIMIT, 0); /* SIGKILL cannot be ignored */
    }

    left = pm_running_count();
    if (left > 0)
        log_event("SHUTDOWN warning: %d child(ren) could not be reaped", left);
    else
        log_event("SHUTDOWN complete: all children stopped and reaped");
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    if (log_open(LOG_PATH) == -1)
        perror("watchdog: cannot open " LOG_PATH " (logging to stderr only)");

    /* Handlers must be in place (and signals blocked) before the first
     * fork(), so no SIGCHLD can be lost. */
    if (sm_install() == -1) {
        log_event("WATCHDOG FATAL: cannot install signal handlers");
        log_close();
        return EXIT_FAILURE;
    }

    log_event("WATCHDOG START pid=%d", (int)getpid());

    int status = EXIT_SUCCESS;
    if (pm_init(children, N_CHILDREN) == -1 || pm_start_all() == -1) {
        log_event("WATCHDOG FATAL: cannot start children");
        status = EXIT_FAILURE;
    } else if (pm_start_timer(CHECK_INTERVAL) == -1) {
        log_event("WATCHDOG warning: periodic check timer not started");
    }

    /* Main control loop. All handled signals are blocked here and only
     * delivered inside sm_wait(), so the flags cannot change between
     * the loop test and going to sleep: no signal is ever lost. */
    while (status == EXIT_SUCCESS && !sm_shutdown_requested()) {
        sm_wait();
        handle_common_events();
        if (sm_take_alarm())
            pm_check();
    }

    graceful_shutdown();
    log_event("WATCHDOG EXIT");
    log_close();
    return status;
}
