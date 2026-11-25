#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <time.h>

/*
 * process_manager - starts, watches and restarts the supervised children.
 *
 * Design:
 *   - This module installs NO signal handlers. signal_manager turns
 *     SIGCHLD / SIGALRM into flags; the main loop then calls pm_reap()
 *     or pm_check(), so all the real work (waitpid, fork, exec, kill,
 *     logging) runs in normal context, never inside a handler.
 *   - Each child runs in its own process group, so Ctrl+C in the
 *     terminal reaches only the watchdog, which then shuts the children
 *     down in a controlled way.
 *   - A child that exits with code 0 is finished (EXITED). A child that
 *     exits with another code or is killed by a signal has crashed and
 *     is restarted, at most MAX_RESTARTS times, then left alone (GAVE_UP).
 */

#define MAX_RESTARTS    3  /* restarts allowed per child after crashes */
#define PM_MAX_CHILDREN 16 /* size of the static child table */

typedef enum {
    CS_RUNNING, /* process is alive                                   */
    CS_PENDING, /* crashed, restart is due but fork() failed: retried */
                /* by the periodic check (pm_check)                   */
    CS_EXITED,  /* finished normally (exit code 0), not restarted     */
    CS_GAVE_UP, /* crashed after MAX_RESTARTS restarts, left alone    */
    CS_STOPPED  /* ended during shutdown                              */
} child_state_t;

/* What to run: main.c builds an array of these and passes it to pm_init(). */
typedef struct {
    const char  *name; /* short name used in the log, e.g. "alpha"        */
    char *const *argv; /* NULL-terminated, argv[0] is the program to exec */
} child_spec_t;

/* One supervised child. Read by logger.c for the status report. */
typedef struct {
    const char   *name;
    char *const  *argv;
    pid_t         pid;        /* current pid, or the last one after it died */
    child_state_t state;
    int           restarts;   /* restarts done so far, 0..MAX_RESTARTS      */
    time_t        started_at; /* time of the last (re)start, for uptime     */
} child_t;

/* Stores the child specs (the pointers must stay valid, e.g. static data).
 * Returns 0 on success, -1 if specs is NULL/invalid or n is out of range. */
int pm_init(const child_spec_t *specs, int n);

/* fork()+execv() every child. Returns 0 if all started, -1 otherwise. */
int pm_start_all(void);

/* Arms setitimer(ITIMER_REAL) so SIGALRM arrives every `seconds`.
 * Returns 0 on success, -1 on error (seconds must be > 0). */
int pm_start_timer(unsigned seconds);

/* Disarms the periodic timer. */
void pm_stop_timer(void);

/* Call after SIGCHLD: reaps every finished child with
 * waitpid(-1, &status, WNOHANG), logs how it ended and restarts it
 * if it crashed and the restart limit is not reached. */
void pm_reap(void);

/* Call after SIGALRM: reaps anything missed, retries pending restarts
 * and logs "CHECK running=N ...". */
void pm_check(void);

/* From now on no child is (re)started and every exit is logged as STOP. */
void pm_begin_shutdown(void);

/* Sends `sig` to every RUNNING child. Returns how many were signalled. */
int pm_signal_all(int sig);

/* Number of children in state CS_RUNNING. */
int pm_running_count(void);

/* The child table and its length (in *n), for the status report. */
const child_t *pm_children(int *n);

/* "RUNNING", "PENDING", "EXITED", "GAVE_UP", "STOPPED" ("UNKNOWN" otherwise). */
const char *pm_state_name(child_state_t s);

#endif
