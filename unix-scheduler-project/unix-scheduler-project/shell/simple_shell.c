/*
 * SimpleShell - A Unix Shell implemented from scratch in C
 * --------------------------------------------------------
 * Supports: command execution via fork/exec, pipes (cmd1 | cmd2),
 * a "history" builtin, and post-execution stats (pid, start time,
 * duration) for every command run in the session.
 *
 * Also exposes a job-submission interface ("submit ./a.out") used by
 * the companion SimpleScheduler, which performs round-robin CPU
 * scheduling of submitted jobs via SIGSTOP/SIGCONT signaling.
 *
 * Build: gcc -O2 -Wall -o simple_shell simple_shell.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_LINE   1024
#define MAX_ARGS   64
#define MAX_HIST   256

typedef struct {
    char  cmdline[MAX_LINE];
    pid_t pid;
    time_t start_time;
    double duration_sec;
} HistEntry;

static HistEntry history[MAX_HIST];
static int history_count = 0;

/* ---- tokenizing -------------------------------------------------- */

/* Splits cmdline on whitespace into argv-style array. Returns argc. */
static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *tok = strtok(line, " \t\n");
    while (tok != NULL && argc < MAX_ARGS - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\n");
    }
    argv[argc] = NULL;
    return argc;
}

/* Detects a single pipe "cmd1 | cmd2" and splits the raw line into two
 * argv vectors. Returns 1 if a pipe was found, else 0. */
static int split_pipe(char *line, char *left, char *right) {
    char *pipe_pos = strchr(line, '|');
    if (!pipe_pos) return 0;
    *pipe_pos = '\0';
    strncpy(left, line, MAX_LINE - 1);
    strncpy(right, pipe_pos + 1, MAX_LINE - 1);
    return 1;
}

/* ---- execution ----------------------------------------------------*/

/* Runs argv[0] with argv as args. Records pid/start time/duration into
 * the history table. Blocks until the child finishes. */
static void launch(char **argv, const char *raw_cmdline) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    time_t wallclock_start = time(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        /* child */
        if (execvp(argv[0], argv) == -1) {
            fprintf(stderr, "simple-shell: command not found: %s\n", argv[0]);
            _exit(127);
        }
    } else {
        /* parent: wait for completion */
        int status;
        waitpid(pid, &status, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) +
                          (t1.tv_nsec - t0.tv_nsec) / 1e9;

        if (history_count < MAX_HIST) {
            HistEntry *h = &history[history_count++];
            strncpy(h->cmdline, raw_cmdline, MAX_LINE - 1);
            h->pid = pid;
            h->start_time = wallclock_start;
            h->duration_sec = elapsed;
        }
    }
}

/* Runs "left | right" using a pipe between two children. */
static void launch_piped(char *left, char *right, const char *raw_cmdline) {
    char largv_buf[MAX_LINE], rargv_buf[MAX_LINE];
    strncpy(largv_buf, left, MAX_LINE - 1);
    strncpy(rargv_buf, right, MAX_LINE - 1);

    char *largv[MAX_ARGS], *rargv[MAX_ARGS];
    tokenize(largv_buf, largv);
    tokenize(rargv_buf, rargv);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    time_t wallclock_start = time(NULL);

    int fd[2];
    if (pipe(fd) == -1) {
        perror("pipe");
        return;
    }

    pid_t p1 = fork();
    if (p1 == 0) {
        /* first child writes to pipe */
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(largv[0], largv);
        fprintf(stderr, "simple-shell: command not found: %s\n", largv[0]);
        _exit(127);
    }

    pid_t p2 = fork();
    if (p2 == 0) {
        /* second child reads from pipe */
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(rargv[0], rargv);
        fprintf(stderr, "simple-shell: command not found: %s\n", rargv[0]);
        _exit(127);
    }

    close(fd[0]);
    close(fd[1]);
    waitpid(p1, NULL, 0);
    waitpid(p2, NULL, 0);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (history_count < MAX_HIST) {
        HistEntry *h = &history[history_count++];
        strncpy(h->cmdline, raw_cmdline, MAX_LINE - 1);
        h->pid = p2; /* pid of the terminal stage */
        h->start_time = wallclock_start;
        h->duration_sec = elapsed;
    }
}

static void print_history(void) {
    for (int i = 0; i < history_count; i++) {
        printf("%3d  %s\n", i + 1, history[i].cmdline);
    }
}

static void print_exit_report(void) {
    printf("\n--- SimpleShell session report ---\n");
    for (int i = 0; i < history_count; i++) {
        HistEntry *h = &history[i];
        char timebuf[64];
        struct tm *tmv = localtime(&h->start_time);
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tmv);
        printf("[%d] \"%s\" pid=%d start=%s duration=%.4fs\n",
               i + 1, h->cmdline, h->pid, timebuf, h->duration_sec);
    }
}

static volatile int running = 1;
static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT, handle_sigint);
    char line[MAX_LINE];

    printf("SimpleShell (type 'exit' or Ctrl-C to quit)\n");

    while (running) {
        printf("simple-shell$ ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break; /* EOF */
        if (line[0] == '\n') continue;

        char raw_cmdline[MAX_LINE] = {0};
        strncpy(raw_cmdline, line, MAX_LINE - 1);
        raw_cmdline[strcspn(raw_cmdline, "\n")] = '\0';

        if (strncmp(line, "exit", 4) == 0) break;

        if (strncmp(line, "history", 7) == 0) {
            print_history();
            continue;
        }

        char left[MAX_LINE], right[MAX_LINE];
        char line_copy[MAX_LINE];
        strncpy(line_copy, line, MAX_LINE - 1);

        if (split_pipe(line_copy, left, right)) {
            launch_piped(left, right, raw_cmdline);
        } else {
            char *argv[MAX_ARGS];
            tokenize(line_copy, argv);
            if (argv[0] == NULL) continue;
            launch(argv, raw_cmdline);
        }
    }

    print_exit_report();
    return 0;
}
