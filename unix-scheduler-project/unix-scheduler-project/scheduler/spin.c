/*
 * spin.c - CPU-bound test job for SimpleScheduler
 * -------------------------------------------------
 * Per assignment rules, scheduled jobs cannot use blocking calls
 * (scanf, sleep, etc.) -- so this busy-loops doing real arithmetic
 * work, printing progress so SIGSTOP/SIGCONT preemption is visible.
 *
 * Build (linked against dummy_main.h, NOT compiled standalone):
 *   gcc -O0 -I../scheduler -o spin spin.c
 */
#include <stdio.h>
#include "dummy_main.h"   /* renames main -> dummy_main, see header */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    volatile long acc = 0;
    for (long i = 0; i < 2000000000L; i++) {
        acc += i % 7;
        if (i % 200000000L == 0) {
            printf("[pid working] iter=%ld acc=%ld\n", i, acc);
            fflush(stdout);
        }
    }
    printf("[pid done] final acc=%ld\n", acc);
    return 0;
}
