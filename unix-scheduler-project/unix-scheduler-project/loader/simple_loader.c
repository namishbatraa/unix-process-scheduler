/*
 * SimpleSmartLoader - Lazy ELF Loader via Page-Fault Handling
 * --------------------------------------------------------------
 * Loads a 32-bit ELF executable WITHOUT mapping any PT_LOAD segment
 * upfront. Jumps directly to the entry point; the resulting SIGSEGV
 * (accessing an unmapped-but-valid address) is intercepted by a
 * SIGSEGV handler, which treats it as a page fault: it looks up which
 * ELF segment owns the faulting address, mmaps exactly one page
 * (4KB) at that address, copies in the corresponding file bytes, and
 * resumes execution transparently.
 *
 * Tracks: total page faults, total page allocations, and internal
 * fragmentation (KB) across the run -- the exact metrics the
 * assignment requires.
 *
 * This is a simplified educational re-implementation of the OS
 * assignment; it does not handle dynamic linking (no glibc) and
 * assumes statically-linked 32-bit ELF binaries, per spec.
 *
 * Build: gcc -O0 -m32 -static -Wall -o loader simple_loader.c
 * (requires 32-bit build support: sudo apt install gcc-multilib)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>

#define PAGE_SIZE 4096

static int      fd;
static Elf32_Ehdr ehdr;
static Elf32_Phdr *phdrs;

static long total_page_faults = 0;
static long total_page_allocations = 0;
static long total_internal_fragmentation_bytes = 0;

/* Finds the PT_LOAD segment that owns the faulting virtual address.
 * Returns its index, or -1 if no segment covers that address. */
static int find_owning_segment(unsigned long fault_addr) {
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        unsigned long seg_start = phdrs[i].p_vaddr;
        unsigned long seg_end   = seg_start + phdrs[i].p_memsz;
        if (fault_addr >= seg_start && fault_addr < seg_end) {
            return i;
        }
    }
    return -1;
}

/* SIGSEGV handler: treats the fault as a "page fault" for a segment
 * that hasn't been mapped yet. Allocates exactly one page, copies in
 * the relevant slice of the file, and returns -- execution resumes
 * at the faulting instruction. */
static void sigsegv_handler(int sig, siginfo_t *si, void *unused) {
    (void)sig; (void)unused;
    unsigned long fault_addr = (unsigned long)si->si_addr;

    int seg_idx = find_owning_segment(fault_addr);
    if (seg_idx < 0) {
        fprintf(stderr, "loader: segfault at %p is not within any PT_LOAD "
                         "segment -- real invalid access, aborting\n",
                si->si_addr);
        _exit(139);
    }

    total_page_faults++;

    Elf32_Phdr *ph = &phdrs[seg_idx];
    unsigned long seg_vaddr = ph->p_vaddr;

    /* Round the fault address down to its page boundary */
    unsigned long page_start = fault_addr - (fault_addr % PAGE_SIZE);

    void *mapped = mmap((void *)page_start, PAGE_SIZE,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1, 0);
    if (mapped == MAP_FAILED) {
        perror("loader: mmap failed during page-fault handling");
        _exit(1);
    }
    total_page_allocations++;

    /* Figure out which bytes of the segment's file image fall on this
     * page, and copy them in. Bytes beyond p_filesz (within p_memsz,
     * e.g. .bss) are left zeroed by the anonymous mapping. */
    unsigned long offset_into_segment = page_start - seg_vaddr;
    unsigned long file_off  = ph->p_offset + offset_into_segment;
    unsigned long copy_len  = PAGE_SIZE;

    if (offset_into_segment < ph->p_filesz) {
        unsigned long remaining_file_bytes = ph->p_filesz - offset_into_segment;
        if (remaining_file_bytes < copy_len) copy_len = remaining_file_bytes;

        char buf[PAGE_SIZE];
        memset(buf, 0, PAGE_SIZE);
        lseek(fd, file_off, SEEK_SET);
        ssize_t r = read(fd, buf, copy_len);
        if (r > 0) memcpy((void *)page_start, buf, r);

        /* Internal fragmentation: the unused tail of this page beyond
         * what the segment actually needed (only meaningful on the
         * last page of the segment). */
        unsigned long seg_end = seg_vaddr + ph->p_memsz;
        unsigned long page_end = page_start + PAGE_SIZE;
        if (page_end > seg_end) {
            total_internal_fragmentation_bytes += (page_end - seg_end);
        }
    }
    /* else: entire page is past p_filesz (pure .bss) -- already zeroed
       by MAP_ANONYMOUS, nothing to copy. */
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <elf32-executable>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr) ||
        memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "loader: not a valid ELF file\n");
        return 1;
    }
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
        fprintf(stderr, "loader: only 32-bit ELF executables are supported\n");
        return 1;
    }

    phdrs = malloc(ehdr.e_phnum * sizeof(Elf32_Phdr));
    lseek(fd, ehdr.e_phoff, SEEK_SET);
    read(fd, phdrs, ehdr.e_phnum * sizeof(Elf32_Phdr));

    /* Install SIGSEGV handler BEFORE jumping to entry point -- every
     * PT_LOAD segment is unmapped at this point. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigsegv_handler;
    sigaction(SIGSEGV, &sa, NULL);

    printf("loader: jumping to entry point 0x%x with zero segments mapped\n",
           ehdr.e_entry);

    /* Typecast entry point to a function pointer and call it directly.
     * The first instruction fetch at e_entry will SIGSEGV (unmapped),
     * triggering lazy loading of the .text page, and so on. */
    int (*entry)(void) = (int (*)(void))(uintptr_t)ehdr.e_entry;
    int ret = entry();

    printf("\n--- SimpleSmartLoader report ---\n");
    printf("Program exit code:           %d\n", ret);
    printf("Total page faults:          %ld\n", total_page_faults);
    printf("Total page allocations:     %ld\n", total_page_allocations);
    printf("Internal fragmentation:     %.2f KB\n",
           total_internal_fragmentation_bytes / 1024.0);

    close(fd);
    free(phdrs);
    return ret;
}
