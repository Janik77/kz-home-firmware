/* Host crypto boundary: real OS/library SHA-256, deterministic IDs only in tests. */
#include <psa/crypto.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/sha.h>
#include <openssl/rand.h>
#endif
int psa_crypto_init(void) { return PSA_SUCCESS; }
int psa_hash_compute(int algorithm, const uint8_t *data, size_t n, uint8_t *out, size_t size, size_t *written)
{
    if (algorithm != PSA_ALG_SHA_256 || size < 32 || n > 4096) return -1;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE provider;
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return -1;
    NTSTATUS result = BCryptHash(provider, NULL, 0, (PUCHAR)data, (ULONG)n, out, 32);
    BCryptCloseAlgorithmProvider(provider, 0);
    if (result) return -1;
#else
    if (!SHA256(data, n, out)) return -1;
#endif
    *written = 32; return PSA_SUCCESS;
}
void esp_fill_random(void *out, size_t size)
{
#ifdef KZ_SERVICE_RANDOM
#ifdef _WIN32
    if (BCryptGenRandom(NULL, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) abort();
#else
    if (RAND_bytes(out, (int)size) != 1) abort();
#endif
#else
    static unsigned counter;
    unsigned char *p = out;
    for (size_t i = 0; i < size; ++i) p[i] = (unsigned char)++counter;
#endif
}
