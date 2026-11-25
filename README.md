# Watchdog

A process supervisor written in C for Linux. The watchdog starts a set of
worker processes, monitors them with POSIX signals, restarts workers that
crash (up to a limit), reports their status on demand and shuts everything
down gracefully.

- Child processes are started with `fork()` + `execv()` and reaped with `waitpid()`.
- All signal handlers are installed with `sigaction()` and only set a
  `volatile sig_atomic_t` flag; the real work happens in the main loop.
- The main loop waits with `sigsuspend()` so no signal can be lost between
  checking the flags and going to sleep.
- Every event is written to `watchdog.log` with `open()` / `write()`.

## Requirements

- Linux (or WSL on Windows)
- `gcc` and `make`
- Optional: `valgrind` for leak checking

## Build

```sh
make            # builds ./watchdog and ./worker, 0 warnings with -Wall -Wextra -pedantic
make clean      # removes binaries, object files and watchdog.log
```

## Run

Run from the project root, because the watchdog execs `./worker`:

```sh
./watchdog
```

The watchdog prints its PID on start. It supervises three test workers:

| Child | Command                     | Expected behaviour                                     |
|-------|-----------------------------|--------------------------------------------------------|
| alpha | `./worker alpha forever`    | runs until stopped                                     |
| beta  | `./worker beta exit-ok 8`   | exits normally after 8 s, not restarted                |
| gamma | `./worker gamma crash 4`    | exits with code 1 every 4 s, restarted 3 times, then given up |

### Sending signals (from a second terminal)

| Action                 | Command                          | Result                                     |
|------------------------|----------------------------------|--------------------------------------------|
| Status report          | `kill -USR1 <watchdog-pid>`      | logs every child's PID, state and uptime   |
| Graceful shutdown      | `kill -TERM <watchdog-pid>`      | SIGTERM to all children, SIGKILL after 5 s |
| Graceful shutdown      | `Ctrl+C` in the watchdog terminal, or `kill -INT <watchdog-pid>` | same as above |
| Force shutdown         | a second `Ctrl+C` / SIGTERM during shutdown | SIGKILL to all children immediately |
| Simulate a crash       | `kill -KILL <worker-pid>`        | logged as CRASH, worker restarted          |
| List the workers       | `ps -o pid,stat,cmd --ppid <watchdog-pid>` | no `Z` (zombie) entries expected |

Workers run in their own process group, so `Ctrl+C` reaches only the
watchdog, which then shuts the workers down itself.

To test the SIGKILL timeout, make the workers ignore SIGTERM:

```sh
WORKER_STUBBORN=1 ./watchdog
```

### The test worker

```
./worker <name> <mode> [seconds]
```

| Mode        | Behaviour                         | Tests scenario                          |
|-------------|-----------------------------------|-----------------------------------------|
| `forever`   | heartbeat every second            | normal operation, SIGKILL -> restart    |
| `exit-ok N` | `exit(0)` after N seconds         | normal termination                      |
| `crash N`   | `exit(1)` after N seconds         | unexpected termination + restart limit  |
| `abort N`   | `abort()` after N seconds         | killed by a signal (SIGABRT)            |

Every worker exits cleanly with code 0 when it receives SIGTERM, unless
`WORKER_STUBBORN=1` is set.

## Tests

```sh
make test
```

`tests/test_scenarios.sh` runs the watchdog three times and prints PASS/FAIL
for each check (exit code 0 only if everything passed):

| # | Scenario               | Checked by                                              |
|---|------------------------|---------------------------------------------------------|
| 1 | Normal operation       | all three children are started                          |
| 2 | Normal termination     | beta exits with code 0 and is not restarted             |
| 3 | Unexpected termination | `kill -KILL` on alpha -> CRASH, RESTART, new PID        |
| 4 | Restart limit          | gamma is restarted exactly 3 times, then GIVE UP; no zombies |
| 5 | Status report          | SIGUSR1 -> STATUS REPORT, watchdog keeps running        |
| 6 | Graceful shutdown      | SIGTERM -> no restarts after shutdown, no children left |
| 7 | SIGINT                 | same shutdown path as Ctrl+C                            |
| 8 | Stubborn workers       | SIGTERM ignored -> SIGKILL after the 5 s timeout        |

Set `SAVE_SAMPLE=1 make test` to copy the log of the first run to
`docs/sample_watchdog.log`. The `Ctrl+C` test is also done by hand, as
described above.

Leak check (optional):

```sh
valgrind --leak-check=full ./watchdog
```

## Log format

Each line in `watchdog.log` (also mirrored to stderr) looks like:

```
[YYYY-MM-DD HH:MM:SS] <EVENT> <details>
```

The log is opened with `O_APPEND`, so earlier runs are kept. Events:

| Event                    | Meaning                                     |
|--------------------------|---------------------------------------------|
| `WATCHDOG START pid=N`   | watchdog started                            |
| `START name pid=N`       | child started                               |
| `EXIT name pid=N normal` | child exited with code 0 (not restarted)    |
| `CRASH name pid=N ...`   | child exited with a non-zero code or was killed by a signal |
| `RESTART name attempt k/3` | child restarted after a crash             |
| `GIVE UP name`           | restart limit reached                       |
| `TIMER periodic check every 5s` | SIGALRM timer armed                  |
| `CHECK running=N ...`    | periodic check (every 5 s, via SIGALRM)     |
| `STATUS REPORT`          | followed by one line per child (SIGUSR1)    |
| `SHUTDOWN requested by SIGxxx` | SIGINT or SIGTERM received            |
| `SHUTDOWN sent SIGTERM ...` | polite stop request sent to all children |
| `STOP name pid=N`        | child stopped during shutdown               |
| `SHUTDOWN timeout ...`   | children did not exit in 5 s, SIGKILL sent  |
| `SHUTDOWN complete ...`  | all children stopped and reaped             |
| `WATCHDOG EXIT`          | watchdog finished                           |
| `LOG CLOSED`             | last line written by the logger             |

Example:

```
[2026-11-25 14:03:00] WATCHDOG START pid=4120
[2026-11-25 14:03:00] START alpha pid=4121
[2026-11-25 14:03:04] CRASH gamma pid=4123 exit code=1
[2026-11-25 14:03:04] RESTART gamma attempt 1/3
[2026-11-25 14:03:11] STATUS REPORT: 3 child(ren), 2 running
[2026-11-25 14:03:11]   alpha  pid=4121   state=RUNNING     restarts=0/3 uptime=11s
```

A full run is in [`docs/sample_watchdog.log`](docs/sample_watchdog.log).

## Project layout

```
Makefile  README.md
include/  signal_manager.h  process_manager.h  logger.h
src/      main.c  signal_manager.c  process_manager.c  logger.c
workers/  worker.c
tests/    test_scenarios.sh
docs/     REPORT.pdf  sample_watchdog.log
```

## Team

| Role | Member | Module | Files |
|------|--------|--------|-------|
| A (Student 1) | Kürşat Kerem ÇEVLİK (KursatKerem56) | Signal management & main program | `signal_manager.h/.c`, `main.c`, `docs/REPORT.pdf` |
| B (Student 2) | İsmail Kerem ERKUL (ismailkeremerkul) | Child process management | `process_manager.h/.c` |
| C (Student 3) | omerfarukbalik | Logging & test infrastructure | `logger.h/.c`, `worker.c`, `Makefile`, `test_scenarios.sh`, `README.md` |

Repository: https://github.com/omerfarukbalik/Watchdog
