// Seqlock reader for the random-number-generator shared block, for use from
// Python (random-number-client.py) via ctypes.
//
// Python has no memory barriers, so on weakly ordered CPUs (ARM, POWER, RISC-V)
// plain Python reads could be reordered and return a value paired with the
// wrong sequence. Here the ordering uses C11 atomics, which emit the right
// barriers on every architecture (dmb ishld / ldar on ARM64, nothing extra
// on x86-64).
//
// Build:
//   cc -O2 -Wall -Wextra -shared -fPIC -o librng_seqlock.so random-number-seqlock.c

#include <stdint.h>

// Same layout as struct ShmData in random-number-generator.c.
struct ShmData {
    uint32_t random_value;
    uint32_t sequence_flag;
};
_Static_assert(sizeof(struct ShmData) == 8, "ShmData must be exactly 8 bytes");

// Read a (value, sequence) pair that the writer published together.
// sequence_flag is odd while a write is in progress, even when stable.
// Retries until it gets the same even sequence before and after the value.
void rng_seqlock_read(const void *base, uint32_t *value_out, uint32_t *seq_out)
{
    const struct ShmData *shm = (const struct ShmData *)base;
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
