#!/usr/bin/python

import ctypes
import os
import platform
import struct
import time
from multiprocessing import shared_memory

# Ensure this matches your C daemon's name ("secure_rng_shm" or "psm_secure_rng_shm")
SHM_NAME = "secure_rng_shm"

VALUE_OFFSET = 0
SEQ_OFFSET = 4

# C seqlock reader with real memory barriers (needed on ARM and other weakly
# ordered CPUs). Build it next to this script with:
#   cc -O2 -Wall -Wextra -shared -fPIC -o librng_seqlock.so random-number-seqlock.c
LIB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "librng_seqlock.so")
X86 = platform.machine().lower() in ("x86_64", "amd64", "i386", "i686")


def _load_lib():
    try:
        lib = ctypes.CDLL(LIB_PATH)
    except OSError:
        return None
    lib.rng_seqlock_read.argtypes = [ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_uint32),
                                     ctypes.POINTER(ctypes.c_uint32)]
    lib.rng_seqlock_read.restype = None
    return lib


_lib = _load_lib()
if _lib is None and not X86:
    raise RuntimeError(
        f"{LIB_PATH} not found. On {platform.machine()} the seqlock read needs "
        "memory barriers that Python cannot provide; build it with:\n"
        "  cc -O2 -Wall -Wextra -shared -fPIC -o librng_seqlock.so random-number-seqlock.c")


def _read_consistent_c(buf):
    """Seqlock read done in C with acquire loads/fences: correct on any CPU."""
    value = ctypes.c_uint32()
    seq = ctypes.c_uint32()
    anchor = ctypes.c_char.from_buffer(buf)  # temporary export, just to get the address
    try:
        _lib.rng_seqlock_read(ctypes.addressof(anchor), ctypes.byref(value), ctypes.byref(seq))
    finally:
        del anchor                           # release the export so shm.close() works
    return value.value, seq.value


def _read_consistent_py(buf):
    """Pure-Python seqlock read, only used on x86-64 when the C helper isn't built.

    Each field is a single aligned 4-byte read, and x86 never reorders loads with
    other loads, so this is safe there without explicit barriers.
    """
    while True:
        seq1, = struct.unpack_from("<I", buf, SEQ_OFFSET)
        if seq1 & 1:              # write in progress
            continue
        value, = struct.unpack_from("<I", buf, VALUE_OFFSET)
        seq2, = struct.unpack_from("<I", buf, SEQ_OFFSET)
        if seq1 == seq2:
            return value, seq1


def read_consistent(buf):
    """Seqlock read: return (value, sequence) that were written together.

    The daemon makes the sequence odd while writing and even when done. We read
    sequence -> value -> sequence and accept only if both sequence reads are the
    same even number, i.e. no write started or finished while we read the value,
    so a mismatched (stale/torn) pair can never be returned.
    """
    return _read_consistent_c(buf) if _lib else _read_consistent_py(buf)


def get_fresh_random():
    try:
        # FIX: Setting track=False stops Python's resource tracker from unlinking
        # and destroying the daemon's shared memory block when this client exits.
        shm = shared_memory.SharedMemory(name=SHM_NAME, create=False, track=False)
    except FileNotFoundError:
        print(f"Error: Shared memory '{SHM_NAME}' not found. Is the C daemon running?")
        return

    last_seen_sequence = None

    try:
        for i in range(5):
            while True:
                random_val, current_sequence = read_consistent(shm.buf)

                if last_seen_sequence is None or current_sequence != last_seen_sequence:
                    last_seen_sequence = current_sequence
                    print(f"[Fetch #{i+1}] Value: {random_val:<10} | Sequence Flag: {current_sequence // 2}")
                    break

                time.sleep(0.00005)

            time.sleep(0.5)

    finally:
        # Closes the local file descriptor cleanly without killing the daemon's device
        shm.close()

if __name__ == "__main__":
    get_fresh_random()

