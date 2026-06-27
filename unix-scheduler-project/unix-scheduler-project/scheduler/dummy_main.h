/*
 * dummy_main.h
 * ------------
 * Every job submitted to SimpleScheduler includes this header. It
 * renames the user's main() to dummy_main() and installs a new main()
 * that pauses the process immediately on startup (via SIGSTOP) so it
 * does not run until SimpleScheduler explicitly signals it (SIGCONT)
 * during its turn in the round-robin queue.
 *
 * This is the synchronization handshake between the scheduler and
 * each scheduled job: the job blocks itself until scheduled, the
 * scheduler unblocks/blocks it in TSLICE-sized bursts.
 */
#ifndef DUMMY_MAIN_H
#define DUMMY_MAIN_H

#include <signal.h>
#include <unistd.h>

int dummy_main(int argc, char **argv);

int main(int argc, char **argv) {
    /* Announce readiness, then pause until the scheduler signals us */
    raise(SIGSTOP);

    int ret = dummy_main(argc, argv);
    return ret;
}

#define main dummy_main

#endif /* DUMMY_MAIN_H */
