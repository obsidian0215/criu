#pragma OPENCL EXTENSION cl_khr_byte_addressable_store : enable
#include "minilzo.h"

#define LZO_BYTE(x)       ((unsigned char) (x))

#define LZO_MAX(a,b)        ((a) >= (b) ? (a) : (b))
#define LZO_MIN(a,b)        ((a) <= (b) ? (a) : (b))
#define LZO_MAX3(a,b,c)     ((a) >= (b) ? LZO_MAX(a,c) : LZO_MAX(b,c))
#define LZO_MIN3(a,b,c)     ((a) <= (b) ? LZO_MIN(a,c) : LZO_MIN(b,c))

#define lzo_sizeof(type)    ((lzo_uint) (sizeof(type)))

#define LZO_HIGH(array)     ((lzo_uint) (sizeof(array)/sizeof(*(array))))

#define LZO_SIZE(bits)      (1u << (bits))
#define LZO_MASK(bits)      (LZO_SIZE(bits) - 1)

#define LZO_USIZE(bits)     ((lzo_uint) 1 << (bits))
#define LZO_UMASK(bits)     (LZO_USIZE(bits) - 1)

#define PTR(a)              ((lzo_uintptr_t) (a))
#define PTR_LINEAR(a)       PTR(a)
#define PTR_ALIGNED_4(a)    ((PTR_LINEAR(a) & 3) == 0)
#define PTR_ALIGNED_8(a)    ((PTR_LINEAR(a) & 7) == 0)
#define PTR_ALIGNED2_4(a,b) (((PTR_LINEAR(a) | PTR_LINEAR(b)) & 3) == 0)
#define PTR_ALIGNED2_8(a,b) (((PTR_LINEAR(a) | PTR_LINEAR(b)) & 7) == 0)

#define PTR_LT(a,b)         (PTR(a) < PTR(b))
#define PTR_GE(a,b)         (PTR(a) >= PTR(b))
#define PTR_DIFF(a,b)       (PTR(a) - PTR(b))
#define pd(a,b)             ((lzo_uint) ((a)-(b)))

#define M1_MAX_OFFSET   0x0400
#define M2_MAX_OFFSET   0x0800
#define M3_MAX_OFFSET   0x4000
#define M4_MAX_OFFSET   0xbfff

#define M1_MIN_LEN      2
#define M1_MAX_LEN      2
#define M2_MIN_LEN      3
#define M2_MAX_LEN      8
#define M3_MIN_LEN      3
#define M3_MAX_LEN      33
#define M4_MIN_LEN      3
#define M4_MAX_LEN      9

#define M1_MARKER       0
#define M2_MARKER       64
#define M3_MARKER       32
#define M4_MARKER       16

#define LZO_BASE 65521u
#define LZO_NMAX 5552

#define TEST_IP               1
#define TEST_OP               1
#define TEST_IP_AND_TEST_OP   1

LZO_PUBLIC(int)
lzo1x_decompress(const lzo_bytep in, lzo_uint  in_len,
    lzo_bytep out, lzo_uintp out_len,
    lzo_voidp wrkmem)
{
    lzo_bytep op;
    const lzo_bytep ip;
    lzo_uint t;
    const lzo_bytep m_pos;

    const lzo_bytep const ip_end = in + in_len;
    LZO_UNUSED(wrkmem);

    * out_len = 0;

    op = out;
    ip = in;

    if (*ip > 17)
    {
        t = *ip++ - 17;
        if (t < 4)
            goto match_next;
        do *op++ = *ip++; while (--t > 0);
        goto first_literal_run;
    }

    for (;;)
    {
        t = *ip++;
        if (t >= 16)
            goto match;
        if (t == 0)
        {
            while (*ip == 0)
            {
                t += 255;
                ip++;
            }
            t += 15 + *ip++;
        }
        *op++ = *ip++; *op++ = *ip++; *op++ = *ip++;
        do *op++ = *ip++; while (--t > 0);
    first_literal_run:
        t = *ip++;
        if (t >= 16)
            goto match;

        m_pos = op - (1 + M2_MAX_OFFSET);
        m_pos -= t >> 2;
        m_pos -= *ip++ << 2;

        *op++ = *m_pos++; *op++ = *m_pos++; *op++ = *m_pos;
        goto match_done;

        for (;;) {
        match:
            if (t >= 64)
            {
                m_pos = op - 1;
                m_pos -= (t >> 2) & 7;
                m_pos -= *ip++ << 3;
                t = (t >> 5) - 1;
                goto copy_match;
            }
            else if (t >= 32)
            {
                t &= 31;
                if (t == 0)
                {
                    while (*ip == 0)
                    {
                        t += 255;
                        ip++;
                    }
                    t += 31 + *ip++;
                }
                m_pos = op - 1;
                m_pos -= (ip[0] >> 2) + (ip[1] << 6);

                ip += 2;
            }
            else if (t >= 16)
            {
                m_pos = op;
                m_pos -= (t & 8) << 11;
                t &= 7;
                if (t == 0)
                {
                    while (*ip == 0)
                    {
                        t += 255;
                        ip++;
                    }
                    t += 7 + *ip++;
                }
                m_pos -= (ip[0] >> 2) + (ip[1] << 6);

                ip += 2;
                if (m_pos == op)
                    goto eof_found;
                m_pos -= 0x4000;
            }
            else
            {
                m_pos = op - 1;
                m_pos -= t >> 2;
                m_pos -= *ip++ << 2;
                *op++ = *m_pos++; *op++ = *m_pos;
                goto match_done;
            }
        copy_match:
            *op++ = *m_pos++; *op++ = *m_pos++;
            do *op++ = *m_pos++; while (--t > 0);

        match_done:
            t = ip[-2] & 3;
            if (t == 0)
                break;

        match_next:
            *op++ = *ip++;
            if (t > 1) { *op++ = *ip++; if (t > 2) { *op++ = *ip++; } }
            t = *ip++;
        }
    }

eof_found:
    *out_len = pd(op, out);
    return (ip == ip_end ? LZO_E_OK :
        (ip < ip_end ? LZO_E_INPUT_NOT_CONSUMED : LZO_E_INPUT_OVERRUN));
}