/*
 * logger.c - timestamped event log for the watchdog (open() + write()).
 *
 * Every line goes to the log file (if open) and to stderr.
 * None of these functions are async-signal-safe: call them only from
 * the main loop, never from a signal handler.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"
#include "process_manager.h"

#define LOG_LINE_MAX 512

static int log_fd = -1;

/* write() may write fewer bytes than requested or be interrupted by a
 * signal; loop until everything is written. Returns 0 / -1. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

int log_open(const char *path)
{
    if (log_fd != -1)
        log_close();

    /* O_APPEND: keep history across runs and make each write() land at the end.
     * O_CLOEXEC: forked workers must not inherit the log descriptor. */
    log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    return log_fd == -1 ? -1 : 0;
}

void log_event(const char *fmt, ...)
{
    int saved_errno = errno; /* callers may still need errno after logging */

    char line[LOG_LINE_MAX];
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;

    if (localtime_r(&now, &tm) == NULL ||
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm) == 0)
        snprintf(ts, sizeof ts, "%ld", (long)now);

    int n = snprintf(line, sizeof line, "[%s] ", ts);
    if (n < 0)
        n = 0;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0)
        n += m;

    /* Truncate over-long messages, always leaving room for the newline. */
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';

    if (log_fd != -1 && write_all(log_fd, line, (size_t)n) == -1) {
        /* Report once, then keep logging to stderr only. */
        perror("logger: write to log file failed");
        close(log_fd);
        log_fd = -1;
    }
    (void)write_all(STDERR_FILENO, line, (size_t)n); /* also show on terminal */

    errno = saved_errno;
}

void log_status_report(void)
{
    int n = 0;
    const child_t *c = pm_children(&n);
    time_t now = time(NULL);

    log_event("STATUS REPORT: %d child(ren), %d running", n, pm_running_count());
    for (int i = 0; i < n; i++) {
        long uptime = c[i].state == CS_RUNNING ? (long)(now - c[i].started_at) : 0L;
        log_event("  %-6s pid=%-6d state=%-11s restarts=%d/%d uptime=%lds",
                  c[i].name, (int)c[i].pid, pm_state_name(c[i].state),
                  c[i].restarts, MAX_RESTARTS, uptime);
    }
}

void log_close(void)
{
    if (log_fd == -1)
        return;
    log_event("LOG CLOSED");
    if (close(log_fd) == -1)
        perror("logger: close");
    log_fd = -1;
}
