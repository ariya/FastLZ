/*
  FastLZ - Byte-aligned LZ77 compression library
  Copyright (C) 2005-2020 Ariya Hidayat <ariya.hidayat@gmail.com>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include "fastlz.h"
#include <stdint.h>

/*
 * Always check for bound when decompressing.
 * Generally it is best to leave it defined.
 */
#define FASTLZ_SAFE

/*
 * Give hints to the compiler for branch prediction optimization.
 */
#if defined(__clang__) || (defined(__GNUC__) && (__GNUC__ > 2))
#define FASTLZ_LIKELY(c) (__builtin_expect(!!(c), 1))
#define FASTLZ_UNLIKELY(c) (__builtin_expect(!!(c), 0))
#else
#define FASTLZ_LIKELY(c) (c)
#define FASTLZ_UNLIKELY(c) (c)
#endif

/*
 * Prevent accessing more than 8-bit at once, except on x86 architectures.
 */
#if !defined(FASTLZ_STRICT_ALIGN)
#define FASTLZ_STRICT_ALIGN
#if defined(__i386__) || defined(__386) /* GNU C, Sun Studio */
#undef FASTLZ_STRICT_ALIGN
#elif defined(__i486__) || defined(__i586__) || defined(__i686__) /* GNU C */
#undef FASTLZ_STRICT_ALIGN
#elif defined(_M_IX86) /* Intel, MSVC */
#undef FASTLZ_STRICT_ALIGN
#elif defined(__386)
#undef FASTLZ_STRICT_ALIGN
#elif defined(_X86_) /* MinGW */
#undef FASTLZ_STRICT_ALIGN
#elif defined(__I86__) /* Digital Mars */
#undef FASTLZ_STRICT_ALIGN
#endif
#endif

#define MAX_COPY 32
#define MAX_LEN 264 /* 256 + 8 */
#define MAX_L1_DISTANCE 8192
#define MAX_L2_DISTANCE 8191
#define MAX_FARDISTANCE (65535 + MAX_L2_DISTANCE - 1)

#if !defined(FASTLZ_STRICT_ALIGN)
#define FASTLZ_READU16(p) *((const uint16_t*)(p))
#else
#define FASTLZ_READU16(p) ((p)[0] | (p)[1] << 8)
#endif

#define HASH_LOG 13
#define HASH_SIZE (1 << HASH_LOG)
#define HASH_MASK (HASH_SIZE - 1)
#define HASH_FUNCTION(v, p)                              \
  {                                                      \
    v = FASTLZ_READU16(p);                               \
    v ^= FASTLZ_READU16(p + 1) ^ (v >> (16 - HASH_LOG)); \
    v &= HASH_MASK;                                      \
  }

int fastlz1_compress(const void* input, int length, void* output) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_bound = ip + length - 2;
  const uint8_t* ip_limit = ip + length - 12 - 1;
  uint8_t* op = (uint8_t*)output;

  const uint8_t* htab[HASH_SIZE];
  const uint8_t** hslot;
  uint32_t hval;

  uint32_t copy;

  /* sanity check */
  if (FASTLZ_UNLIKELY(length < 4)) {
    if (length) {
      /* create literal copy only */
      *op++ = length - 1;
      ip_bound++;
      while (ip <= ip_bound) *op++ = *ip++;
      return length + 1;
    } else
      return 0;
  }

  /* initializes hash table */
  for (hslot = htab; hslot < htab + HASH_SIZE; hslot++) *hslot = ip;

  /* we start with literal copy */
  copy = 2;
  *op++ = MAX_COPY - 1;
  *op++ = *ip++;
  *op++ = *ip++;

  /* main loop */
  while (FASTLZ_LIKELY(ip < ip_limit)) {
    const uint8_t* ref;
    uint32_t distance;

    /* minimum match length */
    uint32_t len = 3;

    /* comparison starting-point */
    const uint8_t* anchor = ip;

    /* find potential match */
    HASH_FUNCTION(hval, ip);
    hslot = htab + hval;
    ref = htab[hval];

    /* calculate distance to the match */
    distance = anchor - ref;

    /* update hash table */
    *hslot = anchor;

    /* is this a match? check the first 3 bytes */
    if (distance == 0 || (distance >= MAX_L1_DISTANCE) || *ref++ != *ip++ ||
        *ref++ != *ip++ || *ref++ != *ip++)
      goto literal;

    /* last matched byte */
    ip = anchor + len;

    /* distance is biased */
    distance--;

    if (!distance) {
      /* zero distance means a run */
      uint8_t x = ip[-1];
      while (ip < ip_bound)
        if (*ref++ != x)
          break;
        else
          ip++;
    } else
      for (;;) {
        /* safe because the outer check against ip limit */
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        while (ip < ip_bound)
          if (*ref++ != *ip++) break;
        break;
      }

    /* if we have copied something, adjust the copy count */
    if (copy) /* copy is biased, '0' means 1 byte copy */
      *(op - copy - 1) = copy - 1;
    else
      /* back, to overwrite the copy count */
      op--;

    /* reset literal counter */
    copy = 0;

    /* length is biased, '1' means a match of 3 bytes */
    ip -= 3;
    len = ip - anchor;

    /* encode the match */
    if (FASTLZ_UNLIKELY(len > MAX_LEN - 2))
      while (len > MAX_LEN - 2) {
        *op++ = (7 << 5) + (distance >> 8);
        *op++ = MAX_LEN - 2 - 7 - 2;
        *op++ = (distance & 255);
        len -= MAX_LEN - 2;
      }

    if (len < 7) {
      *op++ = (len << 5) + (distance >> 8);
      *op++ = (distance & 255);
    } else {
      *op++ = (7 << 5) + (distance >> 8);
      *op++ = len - 7;
      *op++ = (distance & 255);
    }

    /* update the hash at match boundary */
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;

    /* assuming literal copy */
    *op++ = MAX_COPY - 1;

    continue;

  literal:
    *op++ = *anchor++;
    ip = anchor;
    copy++;
    if (FASTLZ_UNLIKELY(copy == MAX_COPY)) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* left-over as literal copy */
  ip_bound++;
  while (ip <= ip_bound) {
    *op++ = *ip++;
    copy++;
    if (copy == MAX_COPY) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* if we have copied something, adjust the copy length */
  if (copy)
    *(op - copy - 1) = copy - 1;
  else
    op--;

  return op - (uint8_t*)output;
}

int fastlz1_decompress(const void* input, int length, void* output,
                       int maxout) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_limit = ip + length;
  uint8_t* op = (uint8_t*)output;
  uint8_t* op_limit = op + maxout;
  uint32_t ctrl = (*ip++) & 31;
  int loop = 1;

  do {
    const uint8_t* ref = op;
    uint32_t len = ctrl >> 5;
    uint32_t ofs = (ctrl & 31) << 8;

    if (ctrl >= 32) {
      len--;
      ref -= ofs;
      if (len == 7 - 1) len += *ip++;
      ref -= *ip++;

#ifdef FASTLZ_SAFE
      if (FASTLZ_UNLIKELY(op + len + 3 > op_limit)) return 0;

      if (FASTLZ_UNLIKELY(ref - 1 < (uint8_t*)output)) return 0;
#endif

      if (FASTLZ_LIKELY(ip < ip_limit))
        ctrl = *ip++;
      else
        loop = 0;

      if (ref == op) {
        /* optimize copy for a run */
        uint8_t b = ref[-1];
        *op++ = b;
        *op++ = b;
        *op++ = b;
        for (; len; --len) *op++ = b;
      } else {
#if !defined(FASTLZ_STRICT_ALIGN)
        const uint16_t* p;
        uint16_t* q;
#endif
        /* copy from reference */
        ref--;
        *op++ = *ref++;
        *op++ = *ref++;
        *op++ = *ref++;

#if !defined(FASTLZ_STRICT_ALIGN)
        /* copy a byte, so that now it's word aligned */
        if (len & 1) {
          *op++ = *ref++;
          len--;
        }

        /* copy 16-bit at once */
        q = (uint16_t*)op;
        op += len;
        p = (const uint16_t*)ref;
        for (len >>= 1; len > 4; len -= 4) {
          *q++ = *p++;
          *q++ = *p++;
          *q++ = *p++;
          *q++ = *p++;
        }
        for (; len; --len) *q++ = *p++;
#else
        for (; len; --len) *op++ = *ref++;
#endif
      }
    } else {
      ctrl++;
#ifdef FASTLZ_SAFE
      if (FASTLZ_UNLIKELY(op + ctrl > op_limit)) return 0;
      if (FASTLZ_UNLIKELY(ip + ctrl > ip_limit)) return 0;
#endif

      *op++ = *ip++;
      for (--ctrl; ctrl; ctrl--) *op++ = *ip++;

      loop = FASTLZ_LIKELY(ip < ip_limit);
      if (loop) ctrl = *ip++;
    }
  } while (FASTLZ_LIKELY(loop));

  return op - (uint8_t*)output;
}

int fastlz2_compress(const void* input, int length, void* output) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_bound = ip + length - 2;
  const uint8_t* ip_limit = ip + length - 12 - 1;
  uint8_t* op = (uint8_t*)output;

  const uint8_t* htab[HASH_SIZE];
  const uint8_t** hslot;
  uint32_t hval;

  uint32_t copy;

  /* sanity check */
  if (FASTLZ_UNLIKELY(length < 4)) {
    if (length) {
      /* create literal copy only */
      *op++ = length - 1;
      ip_bound++;
      while (ip <= ip_bound) *op++ = *ip++;
      return length + 1;
    } else
      return 0;
  }

  /* initializes hash table */
  for (hslot = htab; hslot < htab + HASH_SIZE; hslot++) *hslot = ip;

  /* we start with literal copy */
  copy = 2;
  *op++ = MAX_COPY - 1;
  *op++ = *ip++;
  *op++ = *ip++;

  /* main loop */
  while (FASTLZ_LIKELY(ip < ip_limit)) {
    const uint8_t* ref;
    uint32_t distance;

    /* minimum match length */
    uint32_t len = 3;

    /* comparison starting-point */
    const uint8_t* anchor = ip;

    /* check for a run */
    if (ip[0] == ip[-1] && ip[0] == ip[1] && ip[1] == ip[2]) {
      distance = 1;
      ip += 3;
      ref = anchor - 1 + 3;
      goto match;
    }

    /* find potential match */
    HASH_FUNCTION(hval, ip);
    hslot = htab + hval;
    ref = htab[hval];

    /* calculate distance to the match */
    distance = anchor - ref;

    /* update hash table */
    *hslot = anchor;

    /* is this a match? check the first 3 bytes */
    if (distance == 0 || (distance >= MAX_FARDISTANCE) || *ref++ != *ip++ ||
        *ref++ != *ip++ || *ref++ != *ip++)
      goto literal;

    /* far, needs at least 5-byte match */
    if (distance >= MAX_L2_DISTANCE) {
      if (*ip++ != *ref++ || *ip++ != *ref++) goto literal;
      len += 2;
    }

  match:

    /* last matched byte */
    ip = anchor + len;

    /* distance is biased */
    distance--;

    if (!distance) {
      /* zero distance means a run */
      uint8_t x = ip[-1];
      while (ip < ip_bound)
        if (*ref++ != x)
          break;
        else
          ip++;
    } else
      for (;;) {
        /* safe because the outer check against ip limit */
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        while (ip < ip_bound)
          if (*ref++ != *ip++) break;
        break;
      }

    /* if we have copied something, adjust the copy count */
    if (copy) /* copy is biased, '0' means 1 byte copy */
      *(op - copy - 1) = copy - 1;
    else
      /* back, to overwrite the copy count */
      op--;

    /* reset literal counter */
    copy = 0;

    /* length is biased, '1' means a match of 3 bytes */
    ip -= 3;
    len = ip - anchor;

    /* encode the match */
    if (distance < MAX_L2_DISTANCE) {
      if (len < 7) {
        *op++ = (len << 5) + (distance >> 8);
        *op++ = (distance & 255);
      } else {
        *op++ = (7 << 5) + (distance >> 8);
        for (len -= 7; len >= 255; len -= 255) *op++ = 255;
        *op++ = len;
        *op++ = (distance & 255);
      }
    } else {
      /* far away, but not yet in the another galaxy... */
      if (len < 7) {
        distance -= MAX_L2_DISTANCE;
        *op++ = (len << 5) + 31;
        *op++ = 255;
        *op++ = distance >> 8;
        *op++ = distance & 255;
      } else {
        distance -= MAX_L2_DISTANCE;
        *op++ = (7 << 5) + 31;
        for (len -= 7; len >= 255; len -= 255) *op++ = 255;
        *op++ = len;
        *op++ = 255;
        *op++ = distance >> 8;
        *op++ = distance & 255;
      }
    }

    /* update the hash at match boundary */
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;

    /* assuming literal copy */
    *op++ = MAX_COPY - 1;

    continue;

  literal:
    *op++ = *anchor++;
    ip = anchor;
    copy++;
    if (FASTLZ_UNLIKELY(copy == MAX_COPY)) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* left-over as literal copy */
  ip_bound++;
  while (ip <= ip_bound) {
    *op++ = *ip++;
    copy++;
    if (copy == MAX_COPY) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* if we have copied something, adjust the copy length */
  if (copy)
    *(op - copy - 1) = copy - 1;
  else
    op--;

  /* marker for fastlz2 */
  *(uint8_t*)output |= (1 << 5);

  return op - (uint8_t*)output;
}

int fastlz2_decompress(const void* input, int length, void* output,
                       int maxout) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_limit = ip + length;
  uint8_t* op = (uint8_t*)output;
  uint8_t* op_limit = op + maxout;
  uint32_t ctrl = (*ip++) & 31;
  int loop = 1;

  do {
    const uint8_t* ref = op;
    uint32_t len = ctrl >> 5;
    uint32_t ofs = (ctrl & 31) << 8;

    if (ctrl >= 32) {
      uint8_t code;
      len--;
      ref -= ofs;
      if (len == 7 - 1) do {
          code = *ip++;
          len += code;
        } while (code == 255);
      code = *ip++;
      ref -= code;

      /* match from 16-bit distance */
      if (FASTLZ_UNLIKELY(code == 255))
        if (FASTLZ_LIKELY(ofs == (31 << 8))) {
          ofs = (*ip++) << 8;
          ofs += *ip++;
          ref = op - ofs - MAX_L2_DISTANCE;
        }

#ifdef FASTLZ_SAFE
      if (FASTLZ_UNLIKELY(op + len + 3 > op_limit)) return 0;

      if (FASTLZ_UNLIKELY(ref - 1 < (uint8_t*)output)) return 0;
#endif

      if (FASTLZ_LIKELY(ip < ip_limit))
        ctrl = *ip++;
      else
        loop = 0;

      if (ref == op) {
        /* optimize copy for a run */
        uint8_t b = ref[-1];
        *op++ = b;
        *op++ = b;
        *op++ = b;
        for (; len; --len) *op++ = b;
      } else {
#if !defined(FASTLZ_STRICT_ALIGN)
        const uint16_t* p;
        uint16_t* q;
#endif
        /* copy from reference */
        ref--;
        *op++ = *ref++;
        *op++ = *ref++;
        *op++ = *ref++;

#if !defined(FASTLZ_STRICT_ALIGN)
        /* copy a byte, so that now it's word aligned */
        if (len & 1) {
          *op++ = *ref++;
          len--;
        }

        /* copy 16-bit at once */
        q = (uint16_t*)op;
        op += len;
        p = (const uint16_t*)ref;
        for (len >>= 1; len > 4; len -= 4) {
          *q++ = *p++;
          *q++ = *p++;
          *q++ = *p++;
          *q++ = *p++;
        }
        for (; len; --len) *q++ = *p++;
#else
        for (; len; --len) *op++ = *ref++;
#endif
      }
    } else {
      ctrl++;
#ifdef FASTLZ_SAFE
      if (FASTLZ_UNLIKELY(op + ctrl > op_limit)) return 0;
      if (FASTLZ_UNLIKELY(ip + ctrl > ip_limit)) return 0;
#endif

      *op++ = *ip++;
      for (--ctrl; ctrl; ctrl--) *op++ = *ip++;

      loop = FASTLZ_LIKELY(ip < ip_limit);
      if (loop) ctrl = *ip++;
    }
  } while (FASTLZ_LIKELY(loop));

  return op - (uint8_t*)output;
}

int fastlz_compress(const void* input, int length, void* output) {
  /* for short block, choose fastlz1 */
  if (length < 65536) return fastlz1_compress(input, length, output);

  /* else... */
  return fastlz2_compress(input, length, output);
}

int fastlz_decompress(const void* input, int length, void* output, int maxout) {
  /* magic identifier for compression level */
  int level = ((*(const uint8_t*)input) >> 5) + 1;

  if (level == 1) return fastlz1_decompress(input, length, output, maxout);
  if (level == 2) return fastlz2_decompress(input, length, output, maxout);

  /* unknown level, trigger error */
  return 0;
}

int fastlz_compress_level(int level, const void* input, int length,
                          void* output) {
  if (level == 1) return fastlz1_compress(input, length, output);
  if (level == 2) return fastlz2_compress(input, length, output);

  return 0;
}
