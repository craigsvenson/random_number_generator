// C version of random-number-client.py.
//
// Attaches read-only to the shared memory block published by
// random-number-generator.c and prints 5 fresh random values, each with a
// sequence number the client hasn't seen before, 0.5 s apart.
//
// Uses the same seqlock protocol as the daemon: sequence_flag is odd while a
// write is in progress and even when stable. A (value, sequence) pair is only
// accepted if the same even sequence was seen before and after reading the
// value, so a stale or half-updated value can never be returned. The C11
// atomics emit the right memory barriers on any CPU (x86-64, ARM, ...).
//
// Build:
//   cc -O2 -Wall -Wextra -o random_number_client random-number-client.c
// Run (with ./random_number_generator already running):
//   ./random_number_client

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Must match SHM_NAME in random-number-generator.c
#define SHM_NAME "secure_rng_shm"
#define FETCH_COUNT 5

// Same layout as random-number-generator.c (8 bytes, no padding).
struct ShmData {
    uint32_t random_value;
    uint32_t sequence_flag;
};
_Static_assert(sizeof(struct ShmData) == 8, "ShmData must be exactly 8 bytes");

static void sleep_ns(long ns)
{
    struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

// Seqlock read: returns a (value, sequence) pair that were written together.
static void read_consistent(const struct ShmData *shm, uint32_t *value_out, uint32_t *seq_out)
{
    uint32_t seq1, seq2, value;

    for (;;) {
        // Acquire: the value read below cannot move before this load.
        seq1 = __atomic_load_n(&shm->sequence_flag, __ATOMIC_ACQUIRE);
        if (seq1 & 1u)
            continue;  // writer is mid-update

        value = __atomic_load_n(&shm->random_value, __ATOMIC_RELAXED);

        // Acquire fence: the value read above cannot move after the re-check.
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        seq2 = __atomic_load_n(&shm->sequence_flag, __ATOMIC_RELAXED);

        if (seq1 == seq2)
            break;
    }

    *value_out = value;
    *seq_out = seq1;
}

int main(void)
{
    // Open the daemon's block read-only. No O_CREAT: if it doesn't exist the
    // daemon isn't running. We never shm_unlink() it, so the daemon's block
    // survives this client exiting (the Python version's track=False).
    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (shm_fd == -1) {
        if (errno == ENOENT)
            fprintf(stderr, "Error: Shared memory '%s' not found. Is the C daemon running?\n", SHM_NAME);
        else
            perror("Failed to open shared memory");
        return EXIT_FAILURE;
    }

    const struct ShmData *shm = mmap(NULL, sizeof(struct ShmData), PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);  // the mapping stays valid after closing the descriptor
    if (shm == MAP_FAILED) {
        perror("Failed to mmap shared memory");
        return EXIT_FAILURE;
    }

    uint32_t last_seen_sequence = 0;
    int have_last = 0;

    for (int i = 0; i < FETCH_COUNT; i++) {
        for (;;) {
            uint32_t random_val, current_sequence;
            read_consistent(shm, &random_val, &current_sequence);

            if (!have_last || current_sequence != last_seen_sequence) {
                last_seen_sequence = current_sequence;
                have_last = 1;
                // The sequence goes up by 2 per value (odd = writing), so /2
                // gives the value count, same as the Python client.
                printf("[Fetch #%d] Value: %-10u | Sequence Flag: %u\n",
                       i + 1, random_val, current_sequence / 2);
                fflush(stdout);
                break;
            }

            sleep_ns(50000);       // 50 us, as in the Python client
        }

        sleep_ns(500000000);       // 0.5 s between fetches
    }

    // Unmap only; the daemon's shared memory object is left intact.
    munmap((void *)shm, sizeof(struct ShmData));
    return EXIT_SUCCESS;
}
