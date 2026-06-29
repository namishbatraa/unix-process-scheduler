# Unix Process Scheduler & Shell

A from-scratch Unix shell with an integrated round-robin CPU scheduler,
plus a lazy ELF loader -- originally built as a set of OS coursework
assignments (CSE 231, IIIT-Delhi), reconstructed here as clean reference
implementations.

## Components

### 1. SimpleShell (`shell/simple_shell.c`)
A Unix shell from scratch supporting:
- Command execution via `fork()` + `execvp()`
- Single-stage pipes (`cmd1 | cmd2`)
- `history` builtin
- Per-command session report: pid, start time, wall-clock duration

Tested and verified working: `echo`, `ls`, `wc`, piped commands, and the
exit-time report all produce correct output (see test run below).

### 2. SimpleScheduler (`scheduler/simple_scheduler.c`)
A round-robin CPU scheduler daemon:
- Maintains a FIFO ready queue of submitted jobs
- Each job pauses itself on startup via `raise(SIGSTOP)` (see
  `dummy_main.h`), waiting for the scheduler's signal
- The daemon wakes once per `TSLICE` (sleeps the rest of the time --
  minimal CPU usage between quanta), then:
  1. `SIGSTOP`s the NCPU jobs currently running, requeues them
  2. `SIGCONT`s the next NCPU jobs at the front of the queue
- Tracks per-job completion time and wait time in units of TSLICE

**Verified with a real run:** two CPU-bound jobs (`scheduler/spin.c`)
submitted under `NCPU=1, TSLICE=200ms` show genuinely interleaved
progress -- proof the round-robin preemption is real and not just job
A finishing before job B starts. See `tests/scheduler_run.log`.


## Build & Run

```bash
# Shell
cd shell && gcc -O2 -Wall -o simple_shell simple_shell.c
./simple_shell

# Scheduler (standalone demo against test jobs)
cd scheduler
gcc -O0 -I. -o spin spin.c
gcc -O2 -Wall -o simple_scheduler simple_scheduler.c
./simple_scheduler 1 200 ./spin ./spin   # NCPU=1, TSLICE=200ms

# Loader (requires 32-bit toolchain)
sudo apt install gcc-multilib
cd loader && gcc -m32 -static -O0 -o loader simple_loader.c
./loader ./some_32bit_static_binary
```
