#include <nata/nata_rcsid.h>
__rcsId("$Id: nata_sha1.cpp 6389 2012-07-02 05:12:52Z devtty $")

#include <nata/libnata.h>

/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain

Test Vectors (from FIPS PUB 180-1)
"abc"
  A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
A million repetitions of "a"
  34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
*/


/*
 * #define NATA_LITTLE_ENDIAN
 * This should be #define'd if true.
 */

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/*
 * blk0() and blk() perform the initial expand.
 */
/*
 * I got the idea of expanding during the round function from
 * SSLeay */
#ifdef NATA_LITTLE_ENDIAN
#define blk0(i) \
    (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) \
     |(rol(block->l[i],8)&0x00FF00FF))
#else
#define blk0(i) block->l[i]
#endif /* NATA_LITTLE_ENDIAN */
#define blk(i) \
    (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] \
                          ^block->l[(i+2)&15]^block->l[i&15],1))

/* (R0+R1), R2, R3, R4 are the different operations used in SHA1 */
#define R0(v,w,x,y,z,i) \
    z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) \
    z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) \
    z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) \
    z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) \
    z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);


/*
 * Hash a single 512-bit block. This is the core of the algorithm.
 *	buffer must has 64 bytes.
 */
static void
SHA1Transform(uint32_t state[5], const uint8_t *buffer) {
    uint32_t a, b, c, d, e;
    typedef union {
        uint8_t c[64];
        uint32_t l[16];
    } CHAR64LONG16;
    CHAR64LONG16* block;
    block = (CHAR64LONG16*)buffer;

    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);
    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}



void
nata_initSha1(nata_SHA1_Context *cPtr) {
    cPtr->state[0] = 0x67452301;
    cPtr->state[1] = 0xEFCDAB89;
    cPtr->state[2] = 0x98BADCFE;
    cPtr->state[3] = 0x10325476;
    cPtr->state[4] = 0xC3D2E1F0;
    cPtr->count[0] = cPtr->count[1] = 0;
}


void
nata_updateSha1(nata_SHA1_Context *cPtr,
                const uint8_t *data, size_t len) {
    uint32_t i, j;

    j = (cPtr->count[0] >> 3) & 63;
    if ((cPtr->count[0] += ((uint32_t)len) << 3) < (len << 3)) {
        cPtr->count[1]++;
    }
    cPtr->count[1] += (((uint32_t)len) >> 29);
    if ((j + len) > 63) {
        memcpy((void *)&(cPtr->buffer[j]), (void *)data, (i = 64 - j));
        SHA1Transform(cPtr->state, cPtr->buffer);
        for (; i + 63 < len; i += 64) {
            SHA1Transform(cPtr->state, (const uint8_t *)&data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }

    memcpy((void *)&(cPtr->buffer[j]), (void *)&(data[i]), len - i);
}


void
nata_finalSha1(nata_SHA1_Digest digest, nata_SHA1_Context *cPtr) {
    uint8_t *dPtr = (uint8_t *)digest;
    uint32_t i;
    uint8_t finalcount[8];

    for (i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)
            ((cPtr->count[(i >= 4 ? 0 : 1)] >> ((3-(i & 3)) * 8)) & 255);
        /* Endian independent */
    }
    nata_updateSha1(cPtr, (const uint8_t *)"\200", 1);
    while ((cPtr->count[0] & 504) != 448) {
        nata_updateSha1(cPtr, (const uint8_t *)"\0", 1);
    }
    nata_updateSha1(cPtr, (const uint8_t *)finalcount, 8);
    for (i = 0; i < 20; i++) {
        dPtr[i] = (uint8_t)
            ((cPtr->state[i >> 2] >> ((3-(i & 3)) * 8)) & 255);
    }
}


void
nata_getSha1(const uint8_t *data, size_t len,
             nata_SHA1_Digest digest) {
    nata_SHA1_Context ctx;

    nata_initSha1(&ctx);
    nata_updateSha1(&ctx, data, len);
    nata_finalSha1(digest, &ctx);
}
