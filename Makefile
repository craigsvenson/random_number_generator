# Builds the shared-memory random number demo:
#   random_number_generator  - daemon streaming /dev/urandom into /dev/shm/secure_rng_shm
#   random_number_client     - C client (seqlock reader)
#   librng_seqlock.so        - seqlock reader library used by random-number-client.py
#
#   make            build all three
#   make run        start the daemon, run both clients, stop the daemon
#   make clean      remove build outputs (and a leftover shared memory block)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=

BINS = random_number_generator random_number_client
LIB  = librng_seqlock.so

.PHONY: all run clean

all: $(BINS) $(LIB)

random_number_generator: random-number-generator.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

random_number_client: random-number-client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(LIB): random-number-seqlock.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< $(LDFLAGS)

run: all
	@./random_number_generator >/dev/null & pid=$$!; \
	trap 'kill $$pid 2>/dev/null; wait $$pid 2>/dev/null; rm -f /dev/shm/secure_rng_shm' EXIT; \
	sleep 0.3; \
	echo "--- C client"; ./random_number_client || exit 1; \
	echo "--- Python client"; python3 random-number-client.py

clean:
	rm -f $(BINS) $(LIB)
	rm -f /dev/shm/secure_rng_shm
