#include <include/wally_core.h>
#include <include/wally_bip38.h>
#include <include/wally_crypto.h>
#include "internal.h"
#include "base58.h"
#include "ccan/ccan/crypto/sha256/sha256.h"
#include "ccan/ccan/crypto/ripemd160/ripemd160.h"
#include "ccan/ccan/endian/endian.h"
#include "ccan/ccan/build_assert/build_assert.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "ctaes/ctaes.h"
#include "ctaes/ctaes.c"

#define BIP38_FLAG_DEFAULT   (0x40 | 0x80)
#define BIP38_FLAG_COMPRESSED 0x20
#define BIP38_FLAG_RESERVED1  0x10
#define BIP38_FLAG_RESERVED2  0x08
#define BIP38_FLAG_HAVE_LOT   0x04
#define BIP38_FLAG_RESERVED3  0x02
#define BIP38_FLAG_RESERVED4  0x01
#define BIP38_FLAGS_RESERVED (BIP38_FLAG_RESERVED1 | BIP38_FLAG_RESERVED2 | \
                              BIP38_FLAG_RESERVED3 | BIP38_FLAG_RESERVED4)

#define BITCOIN_PRIVATE_KEY_LEN 32
#define BIP38_DERVIED_KEY_LEN 64u
#define AES256_BLOCK_LEN 16u

#define BIP38_PREFIX   0x01
#define BIP38_ECMUL    0x43
#define BIP38_NO_ECMUL 0x42

struct derived_t {
    unsigned char half1_lo[BIP38_DERVIED_KEY_LEN / 4];
    unsigned char half1_hi[BIP38_DERVIED_KEY_LEN / 4];
    unsigned char half2[BIP38_DERVIED_KEY_LEN / 2];
};

struct bip38_layout_t {
    unsigned char pad1;
    unsigned char prefix;
    unsigned char ec_type;
    unsigned char flags;
    uint32_t hash;
    unsigned char half1[AES256_BLOCK_LEN];
    unsigned char half2[AES256_BLOCK_LEN];
    unsigned char decode_hash[BASE58_CHECKSUM_LEN];
};

/* Check assumptions we expect to hold true */
static void assert_assumptions(void)
{
    /* derived_t/bip38_layout_t must be contiguous */
    BUILD_ASSERT(sizeof(struct derived_t) == BIP38_DERVIED_KEY_LEN);
    /* 44 -> pad1 + 39 + BASE58_CHECKSUM_LEN */
    BUILD_ASSERT(sizeof(struct bip38_layout_t) == 44u);
    BUILD_ASSERT((sizeof(struct bip38_layout_t) - BASE58_CHECKSUM_LEN - 1) ==
                 BIP38_SERIALISED_LEN);
}

/* FIXME: Share this with key_compute_pub_key in bip32.c */
static int compute_pub_key(const unsigned char *bytes_in, size_t len_in,
                           unsigned char *pub_key_out, bool compressed)
{
    secp256k1_pubkey pk;
    const secp256k1_context *ctx = secp_ctx();
    unsigned int flags = compressed ? PUBKEY_COMPRESSED : PUBKEY_UNCOMPRESSED;
    size_t len = compressed ? 33 : 65;
    int ret = len_in == BITCOIN_PRIVATE_KEY_LEN &&
              pubkey_create(ctx, &pk, bytes_in) &&
              pubkey_serialize(ctx, pub_key_out, &len, &pk, flags) ? 0 : -1;
    clear(&pk, sizeof(pk));
    return ret;
}


/* FIXME: Export this with other address functions */
static int address_from_private_key(const unsigned char *bytes_in,
                                    size_t len_in,
                                    unsigned char network,
                                    bool compressed,
                                    char **output)
{
    struct sha256 sha;
    unsigned char pub_key[65];
    struct {
        uint32_t network;
        struct ripemd160 hash160;
        uint32_t checksum;
    } buf;
    unsigned char *network_p = ((unsigned char *)&buf) + 3;
    int ret;

    BUILD_ASSERT(sizeof(buf) == sizeof(struct ripemd160) + sizeof(uint32_t) * 2);

    if (compute_pub_key(bytes_in, len_in, pub_key, compressed))
        return WALLY_EINVAL;

    sha256(&sha, pub_key, compressed ? 33 : 65);
    ripemd160(&buf.hash160, &sha, sizeof(sha));
    *network_p = network;
    buf.checksum = base58_get_checksum(network_p, 1 + 20);
    ret = base58_from_bytes(network_p, 1 + 20 + 4, 0, output);
    clear_n(3, &sha, sizeof(sha), pub_key, sizeof(pub_key), &buf, sizeof(buf));
    return ret;
}

static void aes_enc(const unsigned char *src, const unsigned char *xor,
                    const unsigned char *key, unsigned char *bytes_out)
{
    uint32_t plaintext[AES256_BLOCK_LEN / sizeof(uint32_t)];
    AES256_ctx ctx;
    size_t i;

    if (alignment_ok(src, sizeof(uint32_t)) && alignment_ok(xor, sizeof(uint32_t)))
        for (i = 0; i < sizeof(plaintext) / sizeof(plaintext[0]); ++i)
            plaintext[i] = ((uint32_t *)src)[i] ^ ((uint32_t *)xor)[i];
    else {
        unsigned char *p = (unsigned char *)plaintext;
        for (i = 0; i < sizeof(plaintext); ++i)
            p[i] = src[i] ^ xor[i];
    }

    AES256_init(&ctx, key);
    AES256_encrypt(&ctx, 1, bytes_out, (unsigned char *)plaintext);
    clear_n(2, plaintext, sizeof(plaintext), &ctx, sizeof(ctx));
}

int bip38_raw_from_private_key(const unsigned char *bytes_in, size_t len_in,
                               const unsigned char *pass, size_t pass_len,
                               uint32_t flags,
                               unsigned char *bytes_out, size_t len)
{
    const bool compressed = flags & BIP38_KEY_COMPRESSED;
    struct derived_t derived;
    struct bip38_layout_t buf;
    int ret = WALLY_EINVAL;

    if (!bytes_in || len_in != BITCOIN_PRIVATE_KEY_LEN ||
        !bytes_out || len != BIP38_SERIALISED_LEN)
        goto finish;

    if (flags & BIP38_KEY_RAW_MODE)
        buf.hash = base58_get_checksum(bytes_in, len_in);
    else {
        const unsigned char network = flags & 0xff;
        char *addr58 = NULL;
        if ((ret = address_from_private_key(bytes_in, len_in,
                                            network, compressed, &addr58)))
            goto finish;

        buf.hash = base58_get_checksum((unsigned char *)addr58, strlen(addr58));
        wally_free_string(addr58);
    }

    ret = wally_scrypt(pass, pass_len,
                       (unsigned char *)&buf.hash, sizeof(buf.hash), 16384, 8, 8,
                       (unsigned char *)&derived, sizeof(derived));
    if (ret)
        goto finish;

    buf.prefix = BIP38_PREFIX;
    buf.ec_type = BIP38_NO_ECMUL; /* FIXME: EC-Multiply support */
    buf.flags = BIP38_FLAG_DEFAULT | (compressed ? BIP38_FLAG_COMPRESSED : 0);
    aes_enc(bytes_in + 0, derived.half1_lo, derived.half2, buf.half1);
    aes_enc(bytes_in + 16, derived.half1_hi, derived.half2, buf.half2);

    if (flags & BIP38_KEY_SWAP_ORDER) {
        /* Shuffle hash from the beginning to the end */
        uint32_t tmp = buf.hash;
        memmove(&buf.hash, buf.half1, AES256_BLOCK_LEN * 2);
        memcpy(buf.decode_hash - sizeof(uint32_t), &tmp, sizeof(uint32_t));
    }

    memcpy(bytes_out, &buf.prefix, BIP38_SERIALISED_LEN);

finish:
    clear_n(2, &derived, sizeof(derived), &buf, sizeof(buf));
    return ret;
}

int bip38_from_private_key(const unsigned char *bytes_in, size_t len_in,
                           const unsigned char *pass, size_t pass_len,
                           uint32_t flags, char **output)
{
    struct bip38_layout_t buf;
    int ret;

    if (!output)
        return WALLY_EINVAL;

    *output = NULL;

    ret = bip38_raw_from_private_key(bytes_in, len_in, pass, pass_len,
                                     flags, &buf.prefix, BIP38_SERIALISED_LEN);
    if (!ret)
        ret = base58_from_bytes(&buf.prefix, BIP38_SERIALISED_LEN,
                                BASE58_FLAG_CHECKSUM, output);

    clear(&buf, sizeof(buf));
    return ret;
}


static void aes_dec(const unsigned char *src, const unsigned char *xor,
                    const unsigned char *key, unsigned char *bytes_out)
{
    AES256_ctx ctx;
    size_t i;

    AES256_init(&ctx, key);
    AES256_decrypt(&ctx, 1, bytes_out, src);

    for (i = 0; i < AES256_BLOCK_LEN; ++i)
        bytes_out[i] ^= xor[i];

    clear(&ctx, sizeof(ctx));
}

static int to_private_key(const char *bip38,
                          const unsigned char *bytes_in, size_t len_in,
                          const unsigned char *pass, size_t pass_len,
                          uint32_t flags,
                          unsigned char *bytes_out, size_t len)
{
    struct derived_t derived;
    struct bip38_layout_t buf;
    int ret = WALLY_EINVAL;

    if (len != BITCOIN_PRIVATE_KEY_LEN)
        goto finish;

    if (!(flags & BIP38_KEY_QUICK_CHECK) && !bytes_out)
        goto finish;

    if (bytes_in) {
        if (len_in != BIP38_SERIALISED_LEN)
            goto finish;
        memcpy(&buf.prefix, bytes_in, BIP38_SERIALISED_LEN);
    } else {
        size_t written;
        if ((ret = base58_to_bytes(bip38, BASE58_FLAG_CHECKSUM, &buf.prefix,
                                   BIP38_SERIALISED_LEN + BASE58_CHECKSUM_LEN,
                                   &written)))
            goto finish;

        if (written != BIP38_SERIALISED_LEN) {
            ret = WALLY_EINVAL;
            goto finish;
        }
    }

    if (flags & BIP38_KEY_SWAP_ORDER) {
        /* Shuffle hash from the end to the beginning */
        uint32_t tmp;
        memcpy(&tmp, buf.decode_hash - sizeof(uint32_t), sizeof(uint32_t));
        memmove(buf.half1, &buf.hash, AES256_BLOCK_LEN * 2);
        buf.hash = tmp;
    }

    if (buf.prefix != BIP38_PREFIX ||
        buf.flags & BIP38_FLAGS_RESERVED ||
        buf.ec_type != BIP38_NO_ECMUL /* FIXME: EC Mul support */ ||
        buf.flags & BIP38_FLAG_HAVE_LOT) {
        ret = WALLY_EINVAL;
        goto finish;
    }

    if (flags & BIP38_KEY_QUICK_CHECK) {
        ret = WALLY_OK;
        goto finish;
    }

    if((ret = wally_scrypt(pass, pass_len,
                           (unsigned char *)&buf.hash, sizeof(buf.hash), 16384, 8, 8,
                           (unsigned char *)&derived, sizeof(derived))))
        goto finish;

    aes_dec(buf.half1, derived.half1_lo, derived.half2, bytes_out + 0);
    aes_dec(buf.half2, derived.half1_hi, derived.half2, bytes_out + 16);

    if (flags & BIP38_KEY_RAW_MODE) {
        if (buf.hash != base58_get_checksum(bytes_out, len))
            ret = WALLY_EINVAL;
    } else {
        const unsigned char network = flags & 0xff;
        char *addr58 = NULL;
        ret = address_from_private_key(bytes_out, len, network,
                                       buf.flags & BIP38_FLAG_COMPRESSED, &addr58);
        if (!ret &&
            buf.hash != base58_get_checksum((unsigned char *)addr58, strlen(addr58)))
            ret = WALLY_EINVAL;
        wally_free_string(addr58);
    }

finish:
    clear_n(2, &derived, sizeof(derived), &buf, sizeof(buf));
    return ret;
}

int bip38_raw_to_private_key(const unsigned char *bytes_in, size_t len_in,
                             const unsigned char *pass, size_t pass_len,
                             uint32_t flags,
                             unsigned char *bytes_out, size_t len)
{
    return to_private_key(NULL, bytes_in, len_in, pass, pass_len,
                          flags, bytes_out, len);
}

int bip38_to_private_key(const char *bip38,
                         const unsigned char *pass, size_t pass_len,
                         uint32_t flags,
                         unsigned char *bytes_out, size_t len)
{
    return to_private_key(bip38, NULL, 0, pass, pass_len, flags,
                          bytes_out, len);
}
