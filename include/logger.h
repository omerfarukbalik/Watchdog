#ifndef LOGGER_H
#define LOGGER_H

#define LOG_PATH "watchdog.log"

/* Opens (creates/appends) the log file. Returns 0 on success, -1 on error. */
int  log_open(const char *path);

/* Writes one timestamped line to the log file and to stderr.
 * NOT async-signal-safe: never call it from a signal handler. */
void log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Writes every child's PID + state (triggered by SIGUSR1). */
void log_status_report(void);

/* Writes a final line and closes the log file. */
void log_close(void);

#endif
