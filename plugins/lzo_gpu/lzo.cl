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

#define DMUL(a,b) ((lzo_xint) ((a) * (b)))

#define lzo_memops_TU0p __generic void *
#define lzo_memops_TU1p __generic unsigned char *

#define lzo_memops_set_TU1p     volatile lzo_memops_TU1p
#define lzo_memops_move_TU1p    lzo_memops_TU1p

/* 判断指针运行时对齐 */
static inline bool lzo_ptr_aligned(const void *p, uint align_pow2)
{   return (((ulong) p) & (align_pow2 - 1)) == 0; }

#define LZO_MEMOPS_SET1(dd,cc) \
    LZO_BLOCK_BEGIN \
    *(lzo_memops_set_TU1p) (lzo_memops_TU0p) (dd) = LZO_BYTE(cc); \
    LZO_BLOCK_END

#define LZO_MEMOPS_COPY1(dd,ss)   *((__generic uchar *)(dd)) = *((__generic const uchar *)(ss))
#define LZO_MEMOPS_COPY2(dd,ss)   *((__generic ushort*)(dd)) = *((__generic const ushort*)(ss))

static inline void LZO_MEMOPS_COPY4(void *dd, const void *ss)
{
    if (lzo_ptr_aligned(dd,4) && lzo_ptr_aligned(ss,4))
        *((__generic uint*)dd) =  *((__generic const uint*)ss);
    else {
        uchar4 v = vload4(0, (__generic const uchar*)ss);
        vstore4(v,0,(__generic uchar*)dd);
    }
}

static inline void LZO_MEMOPS_COPY8(void *dd, const void *ss)
{
    if (lzo_ptr_aligned(dd,8) && lzo_ptr_aligned(ss,8))
        *((__generic ulong*)dd) = *((__generic const ulong*)ss);
    else {
        uchar8 v = vload8(0, (__generic const uchar*)ss);
        vstore8(v,0,(__generic uchar*)dd);
    }
}

static inline void LZO_MEMOPS_COPYN(void *dd, const void *ss, uint nn)
{
    __generic uchar *d = (__generic uchar*)dd;
    __generic const uchar *s = (__generic const uchar*)ss;

    /* 先搬 64-bit 带齐尾数 */
    while (nn >= 8 && lzo_ptr_aligned(d,8) && lzo_ptr_aligned(s,8))
    {   LZO_MEMOPS_COPY8(d,s); d+=8; s+=8; nn-=8; }

    /* 再搬 32-bit */
    while (nn >= 4 && lzo_ptr_aligned(d,4) && lzo_ptr_aligned(s,4))
    {   LZO_MEMOPS_COPY4(d,s); d+=4; s+=4; nn-=4; }

    /* 最后逐字节扫尾 */
    for (; nn; --nn) *d++ = *s++;
}

static inline uint lzo_memops_get_le32(const void *pp)
{
    const __generic uchar *p = (__generic const uchar*)pp;

    if (lzo_ptr_aligned(p,4))
        return as_uint(*(__generic const uint*)p);      /* 1 × 32-bit load */

    return  (uint)p[0]        |
           ((uint)p[1] <<  8) |
           ((uint)p[2] << 16) |
           ((uint)p[3] << 24);
}

#define LZO_MEMOPS_GET_LE32(ss)    lzo_memops_get_le32(ss)

#define UA_SET1             LZO_MEMOPS_SET1
#define UA_COPY1            LZO_MEMOPS_COPY1
#define UA_COPY2            LZO_MEMOPS_COPY2
#define UA_COPY4            LZO_MEMOPS_COPY4
#define UA_COPY8            LZO_MEMOPS_COPY8
#define UA_COPYN            LZO_MEMOPS_COPYN
#define UA_GET_LE32         LZO_MEMOPS_GET_LE32

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

#define lzo_dict_t    const lzo_bytep
#define lzo_dict_p    lzo_dict_t *

#define LZO_BASE 65521u
#define LZO_NMAX 5552

#undef  lzo_dict_t
#define lzo_dict_t lzo_uint16_t

#define D_BITS          11
#define DINDEX(dv,p)        DM(((DMUL(0x1824429d,dv)) >> (32-D_BITS)))

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

#define D_SIZE        LZO_SIZE(D_BITS)
#define D_MASK        LZO_MASK(D_BITS)

#define D_HIGH          ((D_MASK >> 1) + 1)

#define DD_BITS       0
#define DD_SIZE         LZO_SIZE(DD_BITS)
#define DD_MASK         LZO_MASK(DD_BITS)

#define DL_BITS       (D_BITS - DD_BITS)
#define DL_SIZE       LZO_SIZE(DL_BITS)
#define DL_MASK       LZO_MASK(DL_BITS)

#undef DM
#undef DX

#define _DV2_A(p,shift1,shift2) \
        (((( (lzo_xint)((p)[0]) << shift1) ^ (p)[1]) << shift2) ^ (p)[2])
#define _DV2_B(p,shift1,shift2) \
        (((( (lzo_xint)((p)[2]) << shift1) ^ (p)[1]) << shift2) ^ (p)[0])
#define _DV3_B(p,shift1,shift2,shift3) \
        ((_DV2_B((p)+1,shift1,shift2) << (shift3)) ^ (p)[0])

#define _DV_A(p,shift)      _DV2_A(p,shift,shift)
#define _DV_B(p,shift)      _DV2_B(p,shift,shift)
#define DA2(p,s1,s2) \
        (((((lzo_xint)((p)[2]) << (s2)) + (p)[1]) << (s1)) + (p)[0])
#define DS2(p,s1,s2) \
        (((((lzo_xint)((p)[2]) << (s2)) - (p)[1]) << (s1)) - (p)[0])
#define DX2(p,s1,s2) \
        (((((lzo_xint)((p)[2]) << (s2)) ^ (p)[1]) << (s1)) ^ (p)[0])
#define DA3(p,s1,s2,s3) ((DA2((p)+1,s2,s3) << (s1)) + (p)[0])
#define DS3(p,s1,s2,s3) ((DS2((p)+1,s2,s3) << (s1)) - (p)[0])
#define DX3(p,s1,s2,s3) ((DX2((p)+1,s2,s3) << (s1)) ^ (p)[0])
#define DMS(v,s)        ((lzo_uint) (((v) & (D_MASK >> (s))) << (s)))
#define DM(v)           DMS(v,0)

// LZO_HASH == LZO_HASH_LZO_INCREMENTAL_B
#define DVAL_FIRST(dv,p)  dv = _DV_B((p),5)
#define DVAL_NEXT(dv,p) \
                dv ^= p[-1]; dv = (((dv) >> 5) ^ ((lzo_xint)(p[2]) << (2*5)))
#define _DINDEX(dv,p)     ((DMUL(0x9f5f,dv)) >> 5)

#define DENTRY(p,in)                          ((lzo_dict_t) pd(p, in))
#define GINDEX(m_pos,m_off,dict,dindex,in)    m_off = dict[dindex]

// DD_BITS == 0
#define UPDATE_D(dict,drun,dv,p,in)       dict[ DINDEX(dv,p) ] = DENTRY(p,in)
#define UPDATE_I(dict,drun,index,p,in)    dict[index] = DENTRY(p,in)
#define UPDATE_P(ptr,drun,p,in)           (ptr)[0] = DENTRY(p,in)

#define LZO_CHECK_MPOS_NON_DET(m_pos,m_off,in,ip,max_offset) \
        (pd(ip, in) <= m_off || \
         ((m_off = pd(ip, in) - m_off) > max_offset) || \
         (m_pos = (ip) - (m_off), 0) )

#define LZO_CHECK_MPOS    LZO_CHECK_MPOS_NON_DET

static lzo_uint
lzo1x_1_compress_core(LZO_ADDR_GLOBAL const lzo_bytep in , lzo_uint  in_len,
                   LZO_ADDR_GLOBAL lzo_bytep out, lzo_uintp out_len,
                    lzo_uint  ti, lzo_voidp wrkmem)
{
    const lzo_bytep ip;
    lzo_bytep op;
    const lzo_bytep const in_end = in + in_len;
    const lzo_bytep const ip_end = in + in_len - 20;
    const lzo_bytep ii;
    lzo_dict_p const dict = (lzo_dict_p) wrkmem;

    op = out;
    ip = in;
    ii = ip;

    ip += ti < 4 ? 4 - ti : 0;
    for (;;)
    {
        const lzo_bytep m_pos;

        lzo_uint m_off;
        lzo_uint m_len;
        {
        lzo_uint32_t dv;
        lzo_uint dindex;
literal:
        ip += 1 + ((ip - ii) >> 5);
next:
        if (ip >= ip_end)
            break;
        dv = UA_GET_LE32(ip);
        dindex = DINDEX(dv,ip);
        GINDEX(m_off,m_pos,in+dict,dindex,in);
        UPDATE_I(dict,0,dindex,ip,in);
        if (dv != UA_GET_LE32(m_pos))
            goto literal;
        }

        ii -= ti; ti = 0;
        lzo_uint t = pd(ip,ii);
        if (t != 0)
        {
            if (t <= 3)
            {
                op[-2] = LZO_BYTE(op[-2] | t);
                { do *op++ = *ii++; while (--t > 0); }
            }
            else
            {
                if (t <= 18)
                    *op++ = LZO_BYTE(t - 3);
                else
                {
                    lzo_uint tt = t - 18;
                    *op++ = 0;
                    while (tt > 255)
                    {
                        tt -= 255;
                        UA_SET1(op, 0);
                        op++;
                    }
                    // assert(tt > 0);
                    *op++ = LZO_BYTE(tt);
                }
                { do *op++ = *ii++; while (--t > 0); }
            }
        }
        m_len = 4;
        if (ip[m_len] == m_pos[m_len]) {
            do {
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip[m_len] != m_pos[m_len])
                    break;
                m_len += 1;
                if (ip + m_len >= ip_end)
                    goto m_len_done;
            } while (ip[m_len] == m_pos[m_len]);
        }
m_len_done:
        m_off = pd(ip,m_pos);
        ip += m_len;
        ii = ip;
        if (m_len <= M2_MAX_LEN && m_off <= M2_MAX_OFFSET)
        {
            m_off -= 1;

            *op++ = LZO_BYTE(((m_len - 1) << 5) | ((m_off & 7) << 2));
            *op++ = LZO_BYTE(m_off >> 3);
        }
        else if (m_off <= M3_MAX_OFFSET)
        {
            m_off -= 1;
            if (m_len <= M3_MAX_LEN)
                *op++ = LZO_BYTE(M3_MARKER | (m_len - 2));
            else
            {
                m_len -= M3_MAX_LEN;
                *op++ = M3_MARKER | 0;
                while(m_len > 255)
                {
                    m_len -= 255;
                    UA_SET1(op, 0);
                    op++;
                }
                *op++ = LZO_BYTE(m_len);
            }
            *op++ = LZO_BYTE(m_off << 2);
            *op++ = LZO_BYTE(m_off >> 6);
        }
        else
        {
            m_off -= 0x4000;
            if (m_len <= M4_MAX_LEN)
                *op++ = LZO_BYTE(M4_MARKER | ((m_off >> 11) & 8) | (m_len - 2));
            else
            {
                m_len -= M4_MAX_LEN;
                *op++ = LZO_BYTE(M4_MARKER | ((m_off >> 11) & 8));
                while(m_len > 255)
                {
                    m_len -= 255;
                    UA_SET1(op, 0);
                    op++;
                }
                *op++ = LZO_BYTE(m_len);
            }
            *op++ = LZO_BYTE(m_off << 2);
            *op++ = LZO_BYTE(m_off >> 6);
        }
        goto next;
    }

    *out_len = pd(op, out);
    return pd(in_end,ii-ti);
}

/* —— memset(dict,0,…) —— */
static inline void dict_clear(uint* d) {
#pragma unroll
    for (uint i = 0; i < D_SIZE; ++i) d[i] = 0;
}

static inline int do_compress(LZO_ADDR_GLOBAL const lzo_bytep in, lzo_uint  in_len,
    LZO_ADDR_GLOBAL lzo_bytep out, lzo_uintp out_len,
    lzo_uint  ti, lzo_voidp wrkmem)
{
    __global const uchar* ip = in;
    __global uchar* op = out;
    lzo_uint l = in_len;
    lzo_uint t = 0;

    while (l > 20)
    {
        lzo_uint ll = LZO_MIN(l, 49152);
        lzo_uintptr_t ll_end = (lzo_uintptr_t)ip + ll;
        if ((ll_end + ((t + ll) >> 5)) <= ll_end ||
            (__global uchar*)(ll_end + ((t + ll) >> 5)) <= ip + ll)
            break;

        dict_clear(wrkmem);
        t = lzo1x_1_compress_core(ip, ll, op, out_len, t, wrkmem);
        ip += ll;
        op += *out_len;
        l -= ll;
    }
    t += l;

    if (t > 0)
    {
        __global const uchar *ii = in + in_len - t;
        if (op == out && t <= 238)
            *op++ = LZO_BYTE(17 + t);
        else if (t <= 3)
            op[-2] = LZO_BYTE(op[-2] | t);
        else if (t <= 18)
            *op++ = LZO_BYTE(t - 3);
        else
        {
            lzo_uint tt = t - 18;
            *op++ = 0;
            while (tt > 255) { tt -= 255; UA_SET1(op, 0); op++; }
            *op++ = LZO_BYTE(tt);
        }
        UA_COPYN(op, ii, t);
        op += t;
    }

    *op++ = M4_MARKER | 1;
    *op++ = 0; *op++ = 0;

    *out_len = pd(op, out);          // 写回实际压缩后长度
    return 0;
}

__kernel void lzo1x_block_compress(__global const uchar *in ,
                                   __global       uchar *out,
                                   __global       uint  *out_len,
	                                        const uint  in_sz,
                                            const uint  blk_size,
                                            const uint  worst_blk)
{
    uint in_len, in_off;
    const uint gid = get_global_id(0);      // 组/线程索引
    in_off = gid * blk_size;
    __global const uchar* ip = in + in_off;
    __global uchar* op = out + gid * worst_blk;

    uint dict[1<<D_BITS];       // 字典: 2k × 4B = 8 KB

    //barrier(CLK_LOCAL_MEM_FENCE);
    in_len = (in_off + blk_size <= in_sz) ? blk_size : (in_sz - in_off);    // 尾块大小
    if (in_len == 0) { 
        out_len[gid] = 0;
        return;
    }

    lzo_uint olen;
    do_compress(ip, in_len, op, &olen, 0, dict);
    out_len[gid]=olen;
}

#define TEST_IP               1
#define TEST_OP               1
#define TEST_IP_AND_TEST_OP   1

static lzo_uint
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

    *out_len = 0;

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

//#define LZO_WRKMEM_SIZE 0

__kernel void lzo1x_block_decompress(
    __global const uchar* in_duf,
    __global const uint* off_arr,       // 各块起始偏移
    __global       uchar* out_buf,
	__global       uint* out_lens,   // 各块解压后长度
    uint blk_sz,
    uint orig_size)
{
    uint gid = get_global_id(0);
    uint in_off = off_arr[gid];
    uint in_len = off_arr[gid + 1] - in_off;

    uint out_off = gid * blk_sz;
    uint out_len = (out_off + blk_sz <= orig_size) ?
        blk_sz : (orig_size - out_off);

    __global const uchar* src = in_duf + in_off;
    __global       uchar* dst = out_buf + out_off;

    //uchar wrkmem[LZO_WRKMEM_SIZE];
    lzo1x_decompress(src, in_len, dst, &out_len, NULL);

    out_lens[gid] = out_len;
}