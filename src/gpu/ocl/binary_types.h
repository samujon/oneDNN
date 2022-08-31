/*******************************************************************************
* Copyright 2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_OCL_BINARY_TYPES_H
#define GPU_OCL_BINARY_TYPES_H
#include "gpu/ocl/ocl_post_ops.h"
#include "gpu/ocl/ocl_types.h"

#undef DST_OFF
#define DST_OFF(x0, x1, x2, x3, x4, x5) OFF_MD(DST, x0, x1, x2, x3, x4, x5)
#define SRC0_OFF(x0, x1, x2, x3, x4, x5) OFF_MD(SRC0, x0, x1, x2, x3, x4, x5)
#define SRC1_OFF(x0, x1, x2, x3, x4, x5) OFF_MD(SRC1, x0, x1, x2, x3, x4, x5)

#if NVECT == 1
#define VECT_INT_TO_FLOAT convert_float
#else
#define VECT_INT_TO_FLOAT CONCAT2(convert_float, NVECT)
#endif

#if SRC0_DT_S8
#define SRC0_BLOCK_READ(src) \
    as_char(intel_sub_group_block_read_uc((const __global uchar *)(src)))
#define SRC0_BLOCK_READ2(src) \
    as_char2(intel_sub_group_block_read_uc2((const __global uchar *)(src)))
#define SRC0_BLOCK_READ4(src) \
    as_char4(intel_sub_group_block_read_uc4((const __global uchar *)(src)))
#define SRC0_BLOCK_READ8(src) \
    as_char8(intel_sub_group_block_read_uc8((const __global uchar *)(src)))
#endif // SRC_DT_S8

#if SRC1_DT_S8
#define SRC1_BLOCK_READ(src) \
    as_char(intel_sub_group_block_read_uc((const __global uchar *)(src)))
#define SRC1_BLOCK_READ2(src) \
    as_char2(intel_sub_group_block_read_uc2((const __global uchar *)(src)))
#define SRC1_BLOCK_READ4(src) \
    as_char4(intel_sub_group_block_read_uc4((const __global uchar *)(src)))
#define SRC1_BLOCK_READ8(src) \
    as_char8(intel_sub_group_block_read_uc8((const __global uchar *)(src)))
#endif // SRC_DT_S8

#if SRC0_DT_U8
#define SRC0_BLOCK_READ(src) \
    as_uchar(intel_sub_group_block_read_uc((const __global uchar *)(src)))
#define SRC0_BLOCK_READ2(src) \
    as_uchar2(intel_sub_group_block_read_uc2((const __global uchar *)(src)))
#define SRC0_BLOCK_READ4(src) \
    as_uchar4(intel_sub_group_block_read_uc4((const __global uchar *)(src)))
#define SRC0_BLOCK_READ8(src) \
    as_uchar8(intel_sub_group_block_read_uc8((const __global uchar *)(src)))
#endif // SRC0_DT_U8

#if SRC1_DT_U8
#define SRC1_BLOCK_READ(src) \
    as_uchar(intel_sub_group_block_read_uc((const __global uchar *)(src)))
#define SRC1_BLOCK_READ2(src) \
    as_uchar2(intel_sub_group_block_read_uc2((const __global uchar *)(src)))
#define SRC1_BLOCK_READ4(src) \
    as_uchar4(intel_sub_group_block_read_uc4((const __global uchar *)(src)))
#define SRC1_BLOCK_READ8(src) \
    as_uchar8(intel_sub_group_block_read_uc8((const __global uchar *)(src)))
#endif // SRC1_DT_U8

#if SRC0_DT_F16
#define SRC0_BLOCK_READ(src) \
    as_half(intel_sub_group_block_read_us((const __global ushort *)(src)))
#define SRC0_BLOCK_READ2(src) \
    as_half2(intel_sub_group_block_read_us2((const __global ushort *)(src)))
#define SRC0_BLOCK_READ4(src) \
    as_half4(intel_sub_group_block_read_us4((const __global ushort *)(src)))
#define SRC0_BLOCK_READ8(src) \
    as_half8(intel_sub_group_block_read_us8((const __global ushort *)(src)))
#endif // SRC0_DT_F16

#if SRC1_DT_F16
#define SRC1_BLOCK_READ(src) \
    as_half(intel_sub_group_block_read_us((const __global ushort *)(src)))
#define SRC1_BLOCK_READ2(src) \
    as_half2(intel_sub_group_block_read_us2((const __global ushort *)(src)))
#define SRC1_BLOCK_READ4(src) \
    as_half4(intel_sub_group_block_read_us4((const __global ushort *)(src)))
#define SRC1_BLOCK_READ8(src) \
    as_half8(intel_sub_group_block_read_us8((const __global ushort *)(src)))
#endif // SRC1_DT_F16

#if SRC0_DT_S32
#define SRC0_BLOCK_READ(src) \
    as_int(intel_sub_group_block_read((const __global uint *)(src)))
#define SRC0_BLOCK_READ2(src) \
    as_int2(intel_sub_group_block_read2((const __global uint *)(src)))
#define SRC0_BLOCK_READ4(src) \
    as_int4(intel_sub_group_block_read4((const __global uint *)(src)))
#define SRC0_BLOCK_READ8(src) \
    as_int8(intel_sub_group_block_read8((const __global uint *)(src)))
#endif // SRC0_DT_S32

#if SRC1_DT_S32
#define SRC1_BLOCK_READ(src) \
    as_int(intel_sub_group_block_read((const __global uint *)(src)))
#define SRC1_BLOCK_READ2(src) \
    as_int2(intel_sub_group_block_read2((const __global uint *)(src)))
#define SRC1_BLOCK_READ4(src) \
    as_int4(intel_sub_group_block_read4((const __global uint *)(src)))
#define SRC1_BLOCK_READ8(src) \
    as_int8(intel_sub_group_block_read8((const __global uint *)(src)))
#endif // SRC1_DT_S32

#if SRC0_DT_F32
#define SRC0_BLOCK_READ(src) \
    as_float(intel_sub_group_block_read((const __global uint *)(src)))
#define SRC0_BLOCK_READ2(src) \
    as_float2(intel_sub_group_block_read2((const __global uint *)(src)))
#define SRC0_BLOCK_READ4(src) \
    as_float4(intel_sub_group_block_read4((const __global uint *)(src)))
#define SRC0_BLOCK_READ8(src) \
    as_float8(intel_sub_group_block_read8((const __global uint *)(src)))
#endif // SRC0_DT_F32

#if SRC1_DT_F32
#define SRC1_BLOCK_READ(src) \
    as_float(intel_sub_group_block_read((const __global uint *)(src)))
#define SRC1_BLOCK_READ2(src) \
    as_float2(intel_sub_group_block_read2((const __global uint *)(src)))
#define SRC1_BLOCK_READ4(src) \
    as_float4(intel_sub_group_block_read4((const __global uint *)(src)))
#define SRC1_BLOCK_READ8(src) \
    as_float8(intel_sub_group_block_read8((const __global uint *)(src)))
#endif // SRC1_DT_F32

#if SRC0_DT_BF16
#define SRC0_BLOCK_READ(src) \
    as_ushort(intel_sub_group_block_read_us((const __global ushort *)(src)))
#define SRC0_BLOCK_READ2(src) \
    as_ushort2(intel_sub_group_block_read_us2((const __global ushort *)(src)))
#define SRC0_BLOCK_READ4(src) \
    as_ushort4(intel_sub_group_block_read_us4((const __global ushort *)(src)))
#define SRC0_BLOCK_READ8(src) \
    as_ushort8(intel_sub_group_block_read_us8((const __global ushort *)(src)))
#endif // SRC0_DT_BF16

#if SRC1_DT_BF16
#define SRC1_BLOCK_READ(src) \
    as_ushort(intel_sub_group_block_read_us((const __global ushort *)(src)))
#define SRC1_BLOCK_READ2(src) \
    as_ushort2(intel_sub_group_block_read_us2((const __global ushort *)(src)))
#define SRC1_BLOCK_READ4(src) \
    as_ushort4(intel_sub_group_block_read_us4((const __global ushort *)(src)))
#define SRC1_BLOCK_READ8(src) \
    as_ushort8(intel_sub_group_block_read_us8((const __global ushort *)(src)))
#endif // SRC1_DT_BF16

#if DST_DT_S8
#define DST_BLOCK_READ(src) \
    as_char(intel_sub_group_block_read_uc((const __global uchar *)(src)))
#define DST_BLOCK_READ2(src) \
    as_char2(intel_sub_group_block_read_uc2((const __global uchar *)(src)))
#define DST_BLOCK_READ4(src) \
    as_char4(intel_sub_group_block_read_uc4((const __global uchar *)(src)))
#define DST_BLOCK_READ8(src) \
    as_char8(intel_sub_group_block_read_uc8((const __global uchar *)(src)))
#define DST_BLOCK_WRITE(dst, val) \
    intel_sub_group_block_write_uc((__global uchar *)(dst), as_uchar(val))
#define DST_BLOCK_WRITE2(dst, val) \
    intel_sub_group_block_write_uc2((__global uchar *)(dst), as_uchar2(val))
#define DST_BLOCK_WRITE4(dst, val) \
    intel_sub_group_block_write_uc4((__global uchar *)(dst), as_uchar4(val))
#define DST_BLOCK_WRITE8(dst, val) \
    intel_sub_group_block_write_uc8((__global uchar *)(dst), as_uchar8(val))
#endif // DST_DT_S8

#if DST_DT_U8
#define DST_BLOCK_READ(src) \
    as_uchar(intel_sub_group_block_read_uc((const __global uchar *)(src)))
#define DST_BLOCK_READ2(src) \
    as_uchar2(intel_sub_group_block_read_uc2((const __global uchar *)(src)))
#define DST_BLOCK_READ4(src) \
    as_uchar4(intel_sub_group_block_read_uc4((const __global uchar *)(src)))
#define DST_BLOCK_READ8(src) \
    as_uchar8(intel_sub_group_block_read_uc8((const __global uchar *)(src)))
#define DST_BLOCK_WRITE(dst, val) \
    intel_sub_group_block_write_uc((__global uchar *)(dst), as_uchar(val))
#define DST_BLOCK_WRITE2(dst, val) \
    intel_sub_group_block_write_uc2((__global uchar *)(dst), as_uchar2(val))
#define DST_BLOCK_WRITE4(dst, val) \
    intel_sub_group_block_write_uc4((__global uchar *)(dst), as_uchar4(val))
#define DST_BLOCK_WRITE8(dst, val) \
    intel_sub_group_block_write_uc8((__global uchar *)(dst), as_uchar8(val))
#endif // SRC_DT_U8

#if DST_DT_F16
#define DST_BLOCK_READ(src) \
    as_half(intel_sub_group_block_read_us((const __global ushort *)(src)))
#define DST_BLOCK_READ2(src) \
    as_half2(intel_sub_group_block_read_us2((const __global ushort *)(src)))
#define DST_BLOCK_READ4(src) \
    as_half4(intel_sub_group_block_read_us4((const __global ushort *)(src)))
#define DST_BLOCK_READ8(src) \
    as_half8(intel_sub_group_block_read_us8((const __global ushort *)(src)))
#define DST_BLOCK_WRITE(dst, val) \
    intel_sub_group_block_write_us((__global ushort *)(dst), as_ushort(val))
#define DST_BLOCK_WRITE2(dst, val) \
    intel_sub_group_block_write_us2((__global ushort *)(dst), as_ushort2(val))
#define DST_BLOCK_WRITE4(dst, val) \
    intel_sub_group_block_write_us4((__global ushort *)(dst), as_ushort4(val))
#define DST_BLOCK_WRITE8(dst, val) \
    intel_sub_group_block_write_us8((__global ushort *)(dst), as_ushort8(val))
#endif // DST_DT_F16

#if DST_DT_S32
#define DST_BLOCK_READ(src) \
    as_int(intel_sub_group_block_read((const __global uint *)(src)))
#define DST_BLOCK_READ2(src) \
    as_int2(intel_sub_group_block_read2((const __global uint *)(src)))
#define DST_BLOCK_READ4(src) \
    as_int4(intel_sub_group_block_read4((const __global uint *)(src)))
#define DST_BLOCK_READ8(src) \
    as_int8(intel_sub_group_block_read8((const __global uint *)(src)))
#define DST_BLOCK_WRITE(dst, val) \
    intel_sub_group_block_write((__global uint *)(dst), as_uint(val))
#define DST_BLOCK_WRITE2(dst, val) \
    intel_sub_group_block_write2((__global uint *)(dst), as_uint2(val))
#define DST_BLOCK_WRITE4(dst, val) \
    intel_sub_group_block_write4((__global uint *)(dst), as_uint4(val))
#define DST_BLOCK_WRITE8(dst, val) \
    intel_sub_group_block_write8((__global uint *)(dst), as_uint8(val))
#endif // DST_DT_S32

#if DST_DT_F32
#define DST_BLOCK_READ(src) \
    as_float(intel_sub_group_block_read((const __global uint *)(src)))
#define DST_BLOCK_READ2(src) \
    as_float2(intel_sub_group_block_read2((const __global uint *)(src)))
#define DST_BLOCK_READ4(src) \
    as_float4(intel_sub_group_block_read4((const __global uint *)(src)))
#define DST_BLOCK_READ8(src) \
    as_float8(intel_sub_group_block_read8((const __global uint *)(src)))
#define DST_BLOCK_WRITE(dst, val) \
    intel_sub_group_block_write((__global uint *)(dst), as_uint(val))
#define DST_BLOCK_WRITE2(dst, val) \
    intel_sub_group_block_write2((__global uint *)(dst), as_uint2(val))
#define DST_BLOCK_WRITE4(dst, val) \
    intel_sub_group_block_write4((__global uint *)(dst), as_uint4(val))
#define DST_BLOCK_WRITE8(dst, val) \
    intel_sub_group_block_write8((__global uint *)(dst), as_uint8(val))
#endif // DST_DT_F32

#if DST_DT_BF16
#define DST_BLOCK_READ(src) \
    as_ushort(intel_sub_group_block_read_us((const __global ushort *)(src)))
#define DST_BLOCK_READ2(src) \
    as_ushort2(intel_sub_group_block_read_us2((const __global ushort *)(src)))
#define DST_BLOCK_READ4(src) \
    as_ushort4(intel_sub_group_block_read_us4((const __global ushort *)(src)))
#define DST_BLOCK_READ8(src) \
    as_ushort8(intel_sub_group_block_read_us8((const __global ushort *)(src)))
#define DST_BLOCK_WRITE(dst, val) \
    intel_sub_group_block_write_us((__global ushort *)(dst), as_ushort(val))
#define DST_BLOCK_WRITE2(dst, val) \
    intel_sub_group_block_write_us2((__global ushort *)(dst), as_ushort2(val))
#define DST_BLOCK_WRITE4(dst, val) \
    intel_sub_group_block_write_us4((__global ushort *)(dst), as_ushort4(val))
#define DST_BLOCK_WRITE8(dst, val) \
    intel_sub_group_block_write_us8((__global ushort *)(dst), as_ushort8(val))
#endif // SRC_DT_F16


#endif
