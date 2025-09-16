#ifndef __MINILZO_H_INCLUDED
#define __MINILZO_H_INCLUDED 1

#define LZO_GPU 1
/* already defined in <cl_kernel.h> */
//typedef unsigned char      uchar;
//typedef unsigned short     ushort;
//typedef unsigned int       uint;
//typedef unsigned long      ulong;

// #include <limits.h>
//#define CHAR_BIT      8
//#define SCHAR_MIN   (-128)
//#define SCHAR_MAX     127
//#define CHAR_MIN    SCHAR_MIN
//#define CHAR_MAX    SCHAR_MAX
//#define UCHAR_MAX     0xff
//
//#define INT_MIN     (-2147483647 - 1)
//#define INT_MAX       2147483647
//#define UINT_MAX      0xffffffff
//#define LONG_MIN    (-9223372036854775807i64 - 1)
//#define LONG_MAX      9223372036854775807i64
//#define ULONG_MAX     0xffffffffffffffffui64

// #include <stddef.h>
//typedef long          ptrdiff_t;
//typedef unsigned long   size_t;

#define LZO_ADDR_GLOBAL  __global
#define LZO_ADDR_LOCAL   __local

#define LZO_PUBLIC(r)  r
#define LZO_EXTERN(r)         extern LZO_PUBLIC(r)
#define LZO_STATIC_CAST(t,e)        ((t) (e))
#define LZO_ITRUNC(t,e)             LZO_STATIC_CAST(t, e)
#define LZO_PP_CONCAT2(a,b)         a ## b
#define LZO_PP_ECONCAT2(a,b)        LZO_PP_CONCAT2(a,b)
#define LZO_BLOCK_BEGIN           do {
#define LZO_BLOCK_END             } while(0)


/* lzo_uint must match size_t */
typedef unsigned long  lzo_uint;
typedef long           lzo_int;
#define LZO_UINT_MAX        ULONG_MAX
#define LZO_INT_MAX         LONG_MAX
#define LZO_INT_MIN         LONG_MIN
#define lzo_xint            lzo_uint

// 64bit (LZO_SIZEOF_LZO_INT64L_T >= LZO_SIZEOF_VOID_P)
#define lzo_intptr_t              long
#define lzo_uintptr_t             unsigned long

#define lzo_bytep               unsigned char *
#define lzo_charp               char *
#define lzo_voidp               void *
#define lzo_shortp              short *
#define lzo_ushortp             unsigned short *
#define lzo_intp                lzo_int *
#define lzo_uintp               lzo_uint *
#define lzo_xintp               lzo_xint *
#define lzo_voidpp              lzo_voidp *
#define lzo_bytepp              lzo_bytep *

#define lzo_int16e_t              short
#define lzo_uint16e_t             unsigned short
#define lzo_int32e_t              int
#define lzo_uint32e_t             unsigned int
#define lzo_int64e_t              long
#define lzo_uint64e_t             unsigned long

#define lzo_int8_t                  signed char
#define lzo_uint8_t                 unsigned char
#define lzo_int16_t                 lzo_int16e_t
#define lzo_uint16_t                lzo_uint16e_t
#define lzo_int32_t                 lzo_int32e_t
#define lzo_uint32_t                lzo_uint32e_t

#define lzo_sizeof_dict_t     ((unsigned)sizeof(lzo_bytep))

typedef union {
        lzo_voidp a00; lzo_bytep a01; lzo_uint a02; lzo_xint a03; lzo_uintptr_t a04;
        void* a05; unsigned char* a06; unsigned long a07; size_t a08; ptrdiff_t a09;
    } lzo_align_t;

/* error codes and prototypes */

#define LZO_E_OK                    0
#define LZO_E_INPUT_OVERRUN         (-4)
#define LZO_E_OUTPUT_OVERRUN        (-5)
#define LZO_E_LOOKBEHIND_OVERRUN    (-6)
#define LZO_E_INPUT_NOT_CONSUMED    (-8)

#define LZO_UNUSED(var)         ((void) &var)

/* Memory required for the wrkmem parameter */

#define LZO1X_MEM_COMPRESS      LZO1X_1_MEM_COMPRESS
/* this version needs 64 KiB work memory */
//#define LZO1X_1_MEM_COMPRESS    ((lzo_uint32_t) (16384L * lzo_sizeof_dict_t))
/* this version needs only 8 KiB work memory */
#define LZO1X_1_MEM_COMPRESS    ((lzo_uint32_t) (2048L * lzo_sizeof_dict_t))
#define LZO1X_MEM_DECOMPRESS    (0)

#endif /* already included */