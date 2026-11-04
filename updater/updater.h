// SHA-256 interface (updater/sha256.c; runtime/net/ldn.c hashes the session id and the room key with it).
//
// This header was missing from the repository: runtime/net/ldn.c and updater/sha256.c both include it, so the Android
// runtime did not compile at all. It declares only what those two files use, rebuilt from sha256.c's own code. The
// PC build's updater (updater/updater.c, which this file's name suggests) is not part of this repository.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];   // the running hash
    uint64_t len;    // bytes taken so far
    uint8_t buf[64]; // the block being filled
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t size);
void sha256_hex(Sha256 *s, char out[65]);  // finishes the hash: 64 hex digits and a NUL
