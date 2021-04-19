/*******************************************************************************
* Copyright 2019-2021 Intel Corporation
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

#include <assert.h>
#include <float.h>
#include <math.h>

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/math_utils.hpp"
#include "common/nstl.hpp"
#include "common/type_helpers.hpp"
#include "cpu/cpu_primitive.hpp"
#include "cpu/simple_q10n.hpp"

#include "cpu/ref_binary.hpp"

namespace dnnl {
namespace impl {
namespace cpu {

template <data_type_t src0_type, data_type_t src1_type, data_type_t dst_type>
status_t ref_binary_t<src0_type, src1_type, dst_type>::execute_ref(
        const exec_ctx_t &ctx) const {
    const auto src0 = CTX_IN_MEM(const src0_data_t *, DNNL_ARG_SRC_0);
    const auto src1 = CTX_IN_MEM(const src1_data_t *, DNNL_ARG_SRC_1);
    auto dst = CTX_OUT_MEM(dst_data_t *, DNNL_ARG_DST);

    const float *scales[2];
    ASSIGN_INPUT_SCALE_VALUE(scales[0], DNNL_ARG_SRC_0);
    ASSIGN_INPUT_SCALE_VALUE(scales[1], DNNL_ARG_SRC_1);

    const memory_desc_wrapper src0_d(pd()->src_md(0));
    const memory_desc_wrapper src1_d(pd()->src_md(1));
    const memory_desc_wrapper dst_d(pd()->dst_md());

    const auto alg = pd()->desc()->alg_kind;

    const auto nelems = dst_d.nelems();
    const auto nelems_pad = dst_d.nelems(true);
    const auto ndims = pd()->ndims();
    const auto has_padding = !dst_d.is_dense();
    const auto has_postops = pd()->attr()->post_ops_.len() != 0;
    const auto is_inplace
            = static_cast<const void *>(src0) == static_cast<void *>(dst);

    if (has_padding && !is_inplace) {
        if (has_postops) {
            // Use zero-padding implementation as we cannot memset over populated memory
            const auto memory = ctx.memory(DNNL_ARG_TO);
            memory->zero_pad(ctx);
        } else {
            const auto res = std::div(
                    static_cast<int>(nelems_pad * dst_d.data_type_size()),
                    PAGE_4K);
            if (!res.quot)
                std::memset(dst, 0, res.rem);
            else
                parallel_nd(res.quot, [&](dim_t i) {
                    const auto tail = (i + 1 == res.quot) ? res.rem : 0;
                    const auto ptr = reinterpret_cast<unsigned char *>(dst)
                            + i * PAGE_4K;
                    std::memset(ptr, 0, PAGE_4K + tail);
                });
        }
    }

    parallel_nd(nelems, [&](dim_t i) {
        dims_t dims_src0, dims_src1; // decomposition for physical offsets
        utils::l_dims_by_l_offset(dims_src0, i, dst_d.dims(), ndims);
        utils::l_dims_by_l_offset(dims_src1, i, dst_d.dims(), ndims);
        auto off_C = dst_d.off_v(dims_src0);

        int mask_src0
                = utils::get_dims_mask(dst_d.dims(), src0_d.dims(), ndims);
        utils::apply_mask_on_dims(dims_src0, ndims, mask_src0);
        const auto off_A = src0_d.off_v(dims_src0);
        int mask_src1
                = utils::get_dims_mask(dst_d.dims(), src1_d.dims(), ndims);
        utils::apply_mask_on_dims(dims_src1, ndims, mask_src1);
        const auto off_B = src1_d.off_v(dims_src1);

        float x_f = (float)src0[off_A];
        float y_f = (float)src1[off_B];
        float dst_f = (float)dst[off_C];

        x_f *= scales[0][0];
        y_f *= scales[1][0];

        float acc = compute_binary_scalar(alg, x_f, y_f);

        if (has_postops) {
            ref_post_ops_t::args_t args;
            args.dst_val = dst_f;
            args.ctx = &ctx;
            args.l_offset = i;
            args.dst_md = pd()->dst_md();
            ref_post_ops->execute(acc, args);
        }

        dst[off_C] = cpu::saturate_and_round<dst_data_t>(acc);
    });

    return status::success;
}

using namespace data_type;

template struct ref_binary_t<f32>;
template struct ref_binary_t<bf16>;
template struct ref_binary_t<s8, s8, s8>;
template struct ref_binary_t<s8, u8, s8>;
template struct ref_binary_t<u8, s8, s8>;
template struct ref_binary_t<u8, u8, s8>;
template struct ref_binary_t<s8, s8, u8>;
template struct ref_binary_t<s8, u8, u8>;
template struct ref_binary_t<u8, s8, u8>;
template struct ref_binary_t<u8, u8, u8>;
template struct ref_binary_t<s8, f32, s8>;
template struct ref_binary_t<s8, f32, u8>;
template struct ref_binary_t<u8, f32, s8>;
template struct ref_binary_t<u8, f32, u8>;
template struct ref_binary_t<f32, s8, s8>;
template struct ref_binary_t<f32, s8, u8>;
template struct ref_binary_t<f32, u8, s8>;
template struct ref_binary_t<f32, u8, u8>;
template struct ref_binary_t<f32, f32, s8>;
template struct ref_binary_t<f32, f32, u8>;

} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
