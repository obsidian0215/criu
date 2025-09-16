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

#define LZO_BASE 65521u
#define LZO_NMAX 5552

#define LZO1X           1
#define LZO_EOF_CODE    1

#define LZO_TEST_OVERRUN 1
#undef DO_DECOMPRESS
#define DO_DECOMPRESS       lzo1x_decompress_safe

#define LZO_TEST_OVERRUN_INPUT       2
#define LZO_TEST_OVERRUN_OUTPUT      2
#define LZO_TEST_OVERRUN_LOOKBEHIND  1

#undef TEST_IP
#undef TEST_OP
#undef TEST_IP_AND_TEST_OP
#undef TEST_LB
#undef TEST_LBO
#undef NEED_IP
#undef NEED_OP
#undef TEST_IV
#undef TEST_OV
#undef HAVE_TEST_IP
#undef HAVE_TEST_OP
#undef HAVE_NEED_IP
#undef HAVE_NEED_OP
#undef HAVE_ANY_IP
#undef HAVE_ANY_OP

#define TEST_IP             (ip < ip_end)
#define NEED_IP(x) \
            if ((lzo_uint)(ip_end - ip) < (lzo_uint)(x))  goto input_overrun
#define TEST_IV(x)          if ((x) >  (lzo_uint)0 - (511)) goto input_overrun

#define NEED_OP(x) \
            if ((lzo_uint)(op_end - op) < (lzo_uint)(x))  goto output_overrun
#define TEST_OV(x)          if ((x) >  (lzo_uint)0 - (511)) goto output_overrun


#  define TEST_LB(m_pos)        if (PTR_LT(m_pos,out) || PTR_GE(m_pos,op)) goto lookbehind_overrun
#  define TEST_LBO(m_pos,o)     if (PTR_LT(m_pos,out) || PTR_GE(m_pos,op-(o))) goto lookbehind_overrun

#define HAVE_TEST_IP 1
#define HAVE_TEST_OP 1
#define TEST_IP_AND_TEST_OP   (TEST_IP && TEST_OP)
#define HAVE_NEED_IP 1
#define HAVE_NEED_OP 1
#define HAVE_ANY_IP 1
#define HAVE_ANY_OP 1

#if defined(DO_DECOMPRESS)
LZO_PUBLIC(int)
DO_DECOMPRESS(const lzo_bytep in, lzo_uint  in_len,
    lzo_bytep out, lzo_uintp out_len,
    lzo_voidp wrkmem)
#endif
{
    lzo_bytep op;
    const lzo_bytep ip;
    lzo_uint t;
    const lzo_bytep m_pos;

    const lzo_bytep const ip_end = in + in_len;
    lzo_bytep const op_end = out + *out_len;

    LZO_UNUSED(wrkmem);

    * out_len = 0;

    op = out;
    ip = in;

    NEED_IP(1);
    if (*ip > 17)
    {
        t = *ip++ - 17;
        if (t < 4)
            goto match_next;
        // assert(t > 0);
        NEED_OP(t); NEED_IP(t + 3);
        do *op++ = *ip++; while (--t > 0);
        goto first_literal_run;
    }

    for (;;)
    {
        NEED_IP(3);
        t = *ip++;
        if (t >= 16)
            goto match;
        if (t == 0)
        {
            while (*ip == 0)
            {
                t += 255;
                ip++;
                TEST_IV(t);
                NEED_IP(1);
            }
            t += 15 + *ip++;
        }
        // assert(t > 0);
        NEED_OP(t + 3); NEED_IP(t + 6);
        {
            *op++ = *ip++; *op++ = *ip++; *op++ = *ip++;
            do *op++ = *ip++; while (--t > 0);
        }

    first_literal_run:

        t = *ip++;
        if (t >= 16)
            goto match;
        m_pos = op - (1 + M2_MAX_OFFSET);
        m_pos -= t >> 2;
        m_pos -= *ip++ << 2;

        TEST_LB(m_pos); NEED_OP(3);
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
                // assert(t > 0);
                TEST_LB(m_pos); NEED_OP(t + 3 - 1);
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
                        TEST_OV(t);
                        NEED_IP(1);
                    }
                    t += 31 + *ip++;
                    NEED_IP(2);
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
                        TEST_OV(t);
                        NEED_IP(1);
                    }
                    t += 7 + *ip++;
                    NEED_IP(2);
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
                TEST_LB(m_pos); NEED_OP(2);
                *op++ = *m_pos++; *op++ = *m_pos;
                goto match_done;
            }

            TEST_LB(m_pos); NEED_OP(t + 3 - 1);
            // assert(t > 0);
            {
            copy_match:
                *op++ = *m_pos++; *op++ = *m_pos++;
                do *op++ = *m_pos++; while (--t > 0);
            }

        match_done:
            t = ip[-2] & 3;
            if (t == 0)
                break;

        match_next:
            // assert(t > 0); assert(t < 4);
            NEED_OP(t); NEED_IP(t + 3);

            * op++ = *ip++;
            if (t > 1) { *op++ = *ip++; if (t > 2) { *op++ = *ip++; } }
            t = *ip++;
            }
        }

eof_found:
    *out_len = pd(op, out);
    return (ip == ip_end ? LZO_E_OK :
        (ip < ip_end ? LZO_E_INPUT_NOT_CONSUMED : LZO_E_INPUT_OVERRUN));

    input_overrun:
    *out_len = pd(op, out);
    return LZO_E_INPUT_OVERRUN;

    output_overrun:
    *out_len = pd(op, out);
    return LZO_E_OUTPUT_OVERRUN;

    lookbehind_overrun:
    *out_len = pd(op, out);
    return LZO_E_LOOKBEHIND_OVERRUN;
}