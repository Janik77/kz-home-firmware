#pragma once
#include <stddef.h>
#include <stdint.h>
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 0x02000009
int psa_crypto_init(void);
int psa_hash_compute(int, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
