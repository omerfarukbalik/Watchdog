/*
 * worker.c - test child process supervised by the watchdog.
 *
 * Usage: ./worker <name> <mode> [seconds]
 *
 *   forever       print a heartbeat every second until SIGTERM
 *   exit-ok N     exit(0) after N seconds   (normal termination)
 *   crash N       exit(1) after N seconds   (unexpected termination)
 *   abort N       abort() after N seconds   (killed by SIGABRT)
 *
 * SIGTERM sets a flag; the main loop then exits cleanly with exit(0).
 * If WORKER_STUBBORN=1 is set, SIGTERM is ignored so the watchdog's
 * SIGKILL timeout path can be tested.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { MODE_FOREVER, MODE_EXIT_OK, MODE_CRASH, MODE_ABORT } mode_t_;

static volatile sig_atomic_t g_stop = 0;

static void on_sigterm(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <name> <mode> [seconds]\n"
            "  modes: forever | exit-ok N | crash N | abort N\n",
            prog);
}

static int parse_mode(const char *s, mode_t_ *out)
{
    if (strcmp(s, "forever") == 0) { *out = MODE_FOREVER;  return 0; }
    if (strcmp(s, "exit-ok") == 0) { *out = MODE_EXIT_OK;  return 0; }
    if (strcmp(s, "crash") == 0)   { *out = MODE_CRASH;    return 0; }
    if (strcmp(s, "abort") == 0)   { *out = MODE_ABORT;    return 0; }
    return -1;
}

/* Parses a non-negative integer number of seconds. Returns -1 on error. */
static long parse_seconds(const char *s)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0)
        return -1;
    return v;
}

static int install_sigterm(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: let sleep() return early on SIGTERM */

    const char *stubborn = getenv("WORKER_STUBBORN");
    sa.sa_handler = (stubborn && strcmp(stubborn, "1") == 0) ? SIG_IGN : on_sigterm;

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("worker: sigaction(SIGTERM)");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    const char *name = argv[1];
    mode_t_ mode;
    if (parse_mode(argv[2], &mode) == -1) {
        fprintf(stderr, "worker: unknown mode '%s'\n", argv[2]);
        usage(argv[0]);
        return 2;
    }

    long seconds = -1; /* -1 = run forever */
    if (mode != MODE_FOREVER) {
        if (argc < 4 || (seconds = parse_seconds(argv[3])) == -1) {
            fprintf(stderr, "worker: mode '%s' needs a seconds argument >= 0\n", argv[2]);
            usage(argv[0]);
            return 2;
        }
    }

    if (install_sigterm() == -1)
        return 2;

    /* Line-buffer stdout so heartbeats show up even when redirected. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("worker %s pid=%d started mode=%s\n", name, (int)getpid(), argv[2]);

    for (long elapsed = 0; !g_stop; elapsed++) {
        if (seconds >= 0 && elapsed >= seconds)
            break;
        printf("worker %s pid=%d heartbeat %lds\n", name, (int)getpid(), elapsed);
        sleep(1); /* returns early if SIGTERM arrives */
    }

    if (g_stop) {
        printf("worker %s pid=%d got SIGTERM, exiting cleanly\n", name, (int)getpid());
        return 0;
    }

    switch (mode) {
    case MODE_EXIT_OK:
        printf("worker %s pid=%d exiting normally (code 0)\n", name, (int)getpid());
        return 0;
    case MODE_CRASH:
        printf("worker %s pid=%d crashing (exit code 1)\n", name, (int)getpid());
        return 1;
    case MODE_ABORT:
        printf("worker %s pid=%d calling abort()\n", name, (int)getpid());
        fflush(stdout);
        abort();
    case MODE_FOREVER:
        break; /* unreachable: forever only ends via SIGTERM */
    }
    return 0;
}
