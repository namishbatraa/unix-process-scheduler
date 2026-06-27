/*
 * SimpleScheduler - A Round-Robin CPU Scheduler Daemon in C
 * -----------------------------------------------------------
 * Companion to simple_shell.c. Implements multi-process scheduling
 * over NCPU "virtual cores" with a fixed time quantum TSLICE (ms).
 *
 * Design:
 *   - SimpleShell forks a child for every "submit ./job" command.
 *     The child immediately raises(SIGSTOP) via dummy_main.h and
 *     waits.
 *   - SimpleShell appends the new pid to a shared ready queue
 *     (a simple circular buffer / FIFO).
 *   - The scheduler daemon wakes up only once per TSLICE (it sleeps
 *     the rest of the time -- "bare minimum CPU cycles" per spec).
 *     On each wake:
 *       1. SIGSTOP the NCPU processes currently running.
 *       2. Move them to the back of the ready queue (round robin).
 *       3. Pop the next NCPU processes from the front of the queue
 *          and SIGCONT them.
 *   - Lifecycle stats (pid, completion time, wait time, in units of
 *     TSLICE) are tracked per job and printed when SimpleShell exits.
 *
 * This demonstrates: process creation/synchronization via POSIX
 * signals, a producer-consumer style ready queue shared between the
 * shell and the scheduler daemon, and round-robin CPU scheduling --
 * the core concurrency/synchronization primitives requested in the
 * Google SWE intern minimum qualifications.
 *
 * Build: gcc -O2 -Wall -o simple_scheduler simple_scheduler.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_JOBS    256
#define MAX_QUEUE   256

typedef enum { JOB_READY, JOB_RUNNING, JOB_DONE } JobState;

typedef struct {
    pid_t       pid;
    char        name[128];
    JobState    state;
    struct timespec arrival;     /* when submitted */
    struct timespec first_run;   /* when first scheduled */
    struct timespec finish;      /* when it exited */
    int         had_first_run;
    int         had_finish;
    long        tslices_waited;  /* time spent in ready queue, in TSLICE units */
    long        tslices_run;     /* completion time, in TSLICE units */
} Job;

static Job   jobs[MAX_JOBS];
static int   job_count = 0;

/* Ready queue: indices into jobs[], FIFO, round-robin */
static int queue[MAX_QUEUE];
static int q_head = 0, q_tail = 0, q_size = 0;

static int NCPU = 1;
static long TSLICE_MS = 100;

static int running_slots[MAX_JOBS]; /* job indices currently on a "CPU" */
static int running_count = 0;

/* ---- queue helpers --------------------------------------------- */

static void q_push(int job_idx) {
    queue[q_tail] = job_idx;
    q_tail = (q_tail + 1) % MAX_QUEUE;
    q_size++;
}

static int q_pop(void) {
    int idx = queue[q_head];
    q_head = (q_head + 1) % MAX_QUEUE;
    q_size--;
    return idx;
}

static double ts_diff_sec(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

/* ---- job submission ---------------------------------------------
 * Called by SimpleShell's "submit ./a.out" handler. Forks the job;
 * the child pauses itself immediately (via dummy_main.h's
 * raise(SIGSTOP)) and waits for the scheduler to SIGCONT it.
 */
static int submit_job(char *const argv[]) {
    if (job_count >= MAX_JOBS) {
        fprintf(stderr, "scheduler: job table full\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        fprintf(stderr, "scheduler: exec failed for %s\n", argv[0]);
        _exit(127);
    }

    /* Parent: wait until the child has stopped itself (raise(SIGSTOP)) */
    int status;
    waitpid(pid, &status, WUNTRACED);

    int idx = job_count++;
    Job *j = &jobs[idx];
    j->pid = pid;
    strncpy(j->name, argv[0], sizeof(j->name) - 1);
    j->state = JOB_READY;
    clock_gettime(CLOCK_MONOTONIC, &j->arrival);
    j->had_first_run = 0;
    j->had_finish = 0;
    j->tslices_waited = 0;
    j->tslices_run = 0;

    q_push(idx);
    printf("scheduler: submitted '%s' as pid %d (queued)\n", j->name, pid);
    return idx;
}

/* ---- one scheduling tick ------------------------------------------
 * Called once per TSLICE by the daemon loop. Stops the currently
 * running jobs, requeues them, then starts up to NCPU jobs from the
 * front of the ready queue.
 */
static void scheduler_tick(long tick_number) {
    /* 1. Stop everything currently running, push to back of queue */
    for (int i = 0; i < running_count; i++) {
        int idx = running_slots[i];
        Job *j = &jobs[idx];

        int status;
        pid_t r = waitpid(j->pid, &status, WNOHANG);
        if (r == j->pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
            /* finished naturally during this slice */
            j->state = JOB_DONE;
            clock_gettime(CLOCK_MONOTONIC, &j->finish);
            j->had_finish = 1;
            j->tslices_run = tick_number - j->tslices_waited + 1;
            continue;
        }

        kill(j->pid, SIGSTOP);
        j->state = JOB_READY;
        q_push(idx);
    }
    running_count = 0;

    /* 2. Pull up to NCPU jobs from the front of the queue and resume them */
    while (running_count < NCPU && q_size > 0) {
        int idx = q_pop();
        Job *j = &jobs[idx];

        if (!j->had_first_run) {
            clock_gettime(CLOCK_MONOTONIC, &j->first_run);
            j->had_first_run = 1;
        }
        j->tslices_waited = tick_number;

        kill(j->pid, SIGCONT);
        j->state = JOB_RUNNING;
        running_slots[running_count++] = idx;
    }
}

/* Returns 1 if all submitted jobs have finished. */
static int all_jobs_done(void) {
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].state != JOB_DONE) return 0;
    }
    return job_count > 0;
}

/* ---- daemon main loop --------------------------------------------
 * Sleeps for TSLICE between ticks -- "bare minimum CPU cycles" as
 * required by the spec: the daemon does no work between quanta.
 */
void run_scheduler_daemon(void) {
    long tick = 0;
    while (job_count == 0 || !all_jobs_done()) {
        struct timespec req = { .tv_sec = TSLICE_MS / 1000,
                                 .tv_nsec = (TSLICE_MS % 1000) * 1000000L };
        nanosleep(&req, NULL);
        if (job_count > 0) {
            scheduler_tick(tick);
            tick++;
        }
    }
}

static void print_final_report(void) {
    printf("\n--- SimpleScheduler final report (NCPU=%d, TSLICE=%ldms) ---\n",
           NCPU, TSLICE_MS);
    printf("%-20s %-8s %-12s %-12s\n", "JOB", "PID", "COMPLETION", "WAIT(xTSLICE)");
    for (int i = 0; i < job_count; i++) {
        Job *j = &jobs[i];
        long wait_slices = j->tslices_run > 0 ? j->tslices_run - 1 : 0;
        printf("%-20s %-8d %-12ldx %-12ldx\n",
               j->name, j->pid,
               j->tslices_run > 0 ? j->tslices_run : 1,
               wait_slices);
    }
}

/* ---- demo entrypoint ----------------------------------------------
 * In the real assignment this is wired up inside SimpleShell via the
 * "submit" builtin. This standalone main demonstrates the scheduler
 * end-to-end against a couple of CPU-bound test jobs (see
 * tests/spin.c) so it can be exercised without the shell.
 */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <NCPU> <TSLICE_ms> [job1] [job2] ...\n", argv[0]);
        fprintf(stderr, "example: %s 2 200 ./spin ./spin\n", argv[0]);
        return 1;
    }
    NCPU = atoi(argv[1]);
    TSLICE_MS = atol(argv[2]);

    for (int i = 3; i < argc; i++) {
        char *job_argv[2] = { argv[i], NULL };
        submit_job(job_argv);
    }

    run_scheduler_daemon();
    print_final_report();
    return 0;
}
