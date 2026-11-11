# Shared-memory random number generator

A small C daemon reads random numbers from `/dev/urandom` and writes them into
POSIX shared memory (`/dev/shm/secure_rng_shm`) about 10,000 times a second.
Clients in C or Python read the latest value directly from that shared memory.

| File | What it is |
|---|---|
| `random-number-generator.c` | The daemon (`random_number_generator`) |
| `random-number-client.c` | C client (`random_number_client`) |
| `random-number-client.py` | Python client |
| `random-number-seqlock.c` | Library (`librng_seqlock.so`) used by the Python client for safe reads |
| `Makefile` | Builds all three |

## Requirements

- Linux (or another system with POSIX shared memory)
- A C compiler (`cc`, `gcc` or `clang`) and `make`
- Python 3.13 or newer for the Python client (it uses `SharedMemory(track=False)`)

## Build

```sh
make
```

This builds `random_number_generator`, `random_number_client` and
`librng_seqlock.so`. Running `make` again rebuilds only what changed.

To choose the compiler or flags:

```sh
make CC=clang CFLAGS="-O3 -Wall"
```

`make clean` removes the build outputs and any leftover
`/dev/shm/secure_rng_shm`.

## Run

Quick demo: this starts the daemon, runs both clients, then stops the daemon
and cleans up:

```sh
make run
```

To run the pieces yourself, start the daemon in one terminal:

```sh
./random_number_generator
```

Then, in another terminal, run either client:

```sh
./random_number_client
python3 random-number-client.py
```

Each client prints 5 fresh values, 0.5 seconds apart:

```
[Fetch #1] Value: 3424989402 | Sequence Flag: 1957
[Fetch #2] Value: 789377328  | Sequence Flag: 5207
...
```

"Sequence Flag" counts how many values the daemon has written so far.

If the daemon isn't running, the clients print
`Error: Shared memory 'secure_rng_shm' not found. Is the C daemon running?`

Stop the daemon with Ctrl+C. It doesn't remove the shared memory block when
it stops. That's harmless: the next start reuses the block, and `make clean`
or `rm /dev/shm/secure_rng_shm` removes it.

## How reads stay consistent

The shared block is 8 bytes: `uint32 random_value` followed by
`uint32 sequence_flag`. It's protected by a **seqlock**, so a client never gets
a value paired with the wrong sequence number:

1. The daemon makes `sequence_flag` odd, writes the new value, then makes the
   flag even again. Atomic stores guarantee this order.
2. A client reads the flag, then the value, then the flag again. It keeps the
   value only if both flag reads are the same **even** number. Otherwise it
   retries.

Because the flag goes up by 2 for each value, the clients display
`sequence_flag / 2`.

The C client and `librng_seqlock.so` use C11 atomics, which include the memory
barriers needed on any CPU, including ARM (for example a Raspberry Pi).

The Python client uses `librng_seqlock.so` when it sits next to the script.
Without the library:

- **x86-64:** the client falls back to a pure-Python read, which is safe there.
- **Other CPUs:** the client stops with an error telling you to build the
  library (`make`), because Python can't add memory barriers itself.
