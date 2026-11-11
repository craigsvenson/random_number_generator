
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SHM_NAME "secure_rng_shm"

// Shared layout, matching Python's "<II" (8 bytes, naturally aligned, no padding).
//
// sequence_flag is a seqlock: it is odd while a write is in progress and even
// when random_value is stable. A reader takes a value only if it saw the same
// even sequence before and after reading it, so it can never get a value
// paired with the wrong sequence (a "stale" or half-updated value).
struct ShmData {
    uint32_t random_value;
    uint32_t sequence_flag;
};
_Static_assert(sizeof(struct ShmData) == 8, "ShmData must be exactly 8 bytes");

int main() {
    int urandom_fd = -1;
    int shm_fd = -1;
    struct ShmData *shared_data = NULL;
    uint32_t sequence_counter = 0;  // always even between writes

    // 1. Open /dev/urandom for reading cryptographically secure bytes
    urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0) {
        perror("Failed to open /dev/urandom");
        return EXIT_FAILURE;
    }

    // 2. Create the POSIX shared memory object
    // O_CREAT: create it if it doesn't exist; O_RDWR: open for read/write
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        close(urandom_fd);
        return EXIT_FAILURE;
    }

    // 3. Set the shared memory allocation size to exactly 8 bytes
    if (ftruncate(shm_fd, sizeof(struct ShmData)) == -1) {
        perror("Failed to resize shared memory");
        close(urandom_fd);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }

    // 4. Map the shared memory object into our process's memory space
    shared_data = (struct ShmData *)mmap(NULL, sizeof(struct ShmData),
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, shm_fd, 0);
    if (shared_data == MAP_FAILED) {
        perror("Failed to mmap shared memory");
        close(urandom_fd);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }

    // Reset the seqlock: a block left over from a killed run could hold an odd
    // (mid-write) sequence, which would make readers wait forever.
    __atomic_store_n(&shared_data->sequence_flag, 0, __ATOMIC_RELEASE);

    printf("C RNG Daemon started successfully. Streaming /dev/urandom to /dev/shm/%s\n", SHM_NAME);
    printf("Press Ctrl+C to terminate.\n");

    // 5. Main loop: Continuously populate the shared memory
    while (1) {
        uint32_t fresh_random;

        // Read 4 bytes of secure entropy into a temporary variable
        if (read(urandom_fd, &fresh_random, sizeof(fresh_random)) == sizeof(fresh_random)) {

            // Seqlock write: odd = write in progress.
            __atomic_store_n(&shared_data->sequence_flag, ++sequence_counter, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_RELEASE);   // odd flag visible before the value changes

            __atomic_store_n(&shared_data->random_value, fresh_random, __ATOMIC_RELAXED);

            // Even again = value complete. Release orders the value store before it.
            __atomic_store_n(&shared_data->sequence_flag, ++sequence_counter, __ATOMIC_RELEASE);
        }

        // Extremely short micro-sleep (100 microseconds)
        // Keeps CPU usage virtually zero while maintaining lightning-fast RNG rotation
        usleep(100);
    }

    // Clean up code (Unreachable due to infinite loop, handles OS-level cleanup via process destruction)
    munmap(shared_data, sizeof(struct ShmData));
    close(shm_fd);
    close(urandom_fd);
    return EXIT_SUCCESS;
}

