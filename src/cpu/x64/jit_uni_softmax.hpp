/*******************************************************************************
* Copyright 2019-2023 Intel Corporation
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

#ifndef CPU_X64_JIT_UNI_SOFTMAX_HPP
#define CPU_X64_JIT_UNI_SOFTMAX_HPP

#include <assert.h>

#include "common/c_types_map.hpp"
#include "common/memory_tracking.hpp"
#include "common/primitive.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"

#include "cpu/cpu_softmax_pd.hpp"

#include "cpu/x64/cpu_isa_traits.hpp"
#include "cpu/x64/injectors/jit_uni_postops_injector.hpp"
#include "cpu/x64/jit_avx512_core_bf16cvt.hpp"

#define VCHECK_SOFTMAX(cond, msg, ...) \
    VCONDCHECK(create, dispatch, softmax, (cond), status::unimplemented, \
            "%s," msg, this->info(engine), ##__VA_ARGS__)

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

namespace softmax_impl {
struct driver_t;

bcast_set_t get_supported_bcast_strategies();
} // namespace softmax_impl

struct jit_uni_softmax_fwd_t : public primitive_t {
    struct pd_t : public cpu_softmax_fwd_pd_t {
        using cpu_softmax_fwd_pd_t::cpu_softmax_fwd_pd_t;

        DECLARE_COMMON_PD_T("jit:uni", jit_uni_softmax_fwd_t);

        status_t init(engine_t *engine) {
            auto is_dense = [&]() {
                const memory_desc_wrapper src_d(src_md());
                const auto &bd = src_d.blocking_desc();

                if (!src_d.is_dense(true) || !src_d.only_padded_dim(axis()))
                    return false;

                if (src_d.is_plain()) return bd.strides[axis()] == 1;

                // It is fine to use float here as the kernel uses halfs of
                // vector registers.
                const dim_t blk_size
                        = isa_max_vlen(get_max_cpu_isa()) / sizeof(float);
                // 31 is a general limit, 2 is for unroll_regs_ = 4;
                const size_t max_stride = (1LL << (31 - 2)) - 1;
                const int last_blk = bd.inner_nblks - 1;
                return bd.inner_blks[last_blk] == blk_size
                        && bd.inner_idxs[last_blk] == axis()
                        && sizeof(float) * bd.strides[axis()] < max_stride;
            };

            using namespace data_type;
            using skip_mask_t = primitive_attr_t::skip_mask_t;

            const auto src_dt = src_md()->data_type;
            const auto dst_dt = dst_md()->data_type;
            bool ok = is_fwd() && !has_zero_dim_memory()
                    && utils::one_of(src_dt, f32, bf16, f16, s8, u8)
                    && utils::one_of(dst_dt, f32, bf16, f16, s8, u8)
                    // s8/u8 are temporary limitations due to priorities
                    && IMPLICATION(
                            (utils::one_of(s8, src_dt, dst_dt)
                                    || utils::one_of(u8, src_dt, dst_dt)),
                            mayiuse(avx512_core))
                    && IMPLICATION(utils::one_of(bf16, src_dt, dst_dt),
                            mayiuse(avx512_core) || mayiuse(avx2_vnni_2))
                    && IMPLICATION(utils::one_of(f16, src_dt, dst_dt),
                            mayiuse(avx512_core_fp16) || mayiuse(avx2_vnni_2));
            if (!ok) return status::unimplemented;

            VCHECK_SOFTMAX(
                    attr()->has_default_values(skip_mask_t::scales_runtime
                            | skip_mask_t::post_ops),
                    VERBOSE_UNSUPPORTED_ATTR);
            VCHECK_SOFTMAX(attr_scales_ok(), VERBOSE_UNSUPPORTED_SCALES_CFG);
            VCHECK_SOFTMAX(post_ops_ok(), VERBOSE_UNSUPPORTED_POSTOP);
#undef VCHECK_SOFTMAX

            ok = set_default_formats() == status::success
                    && attr_.set_default_formats(dst_md(0)) == status::success;
            if (!ok) return status::unimplemented;

            ok = memory_desc_wrapper(src_md()).similar_to(
                         memory_desc_wrapper(dst_md()), true, false, 0)
                    && is_dense(); // not dense impl can be easily done
            if (!ok) return status::unimplemented;

            // AVX2 only supports xf16 on plain layout now
            ok = IMPLICATION(mayiuse(avx2_vnni_2) && !mayiuse(avx512_core)
                            && (utils::one_of(bf16, src_dt, dst_dt)
                                    || utils::one_of(f16, src_dt, dst_dt)),
                    memory_desc_wrapper(src_md()).is_plain());
            if (!ok) return status::unimplemented;

            nthr_ = dnnl_get_max_threads();
            init_scratchpad();

            return status::success;
        };

        int nthr_; // To not exceed the limit in execute used for set up.

    private:
        void init_scratchpad() {
            if (utils::one_of(
                        dst_md()->data_type, data_type::u8, data_type::s8)) {
                auto scratchpad = scratchpad_registry().registrar();
                scratchpad.template book<char>(
                        memory_tracking::names::key_softmax_interim_store,
                        axis_size(true) * sizeof(float) * nthr_);
            }
        }

        bool post_ops_ok() const {
            const auto &post_ops = attr()->post_ops_;
            const bool with_sum = post_ops.find(primitive_kind::sum) != -1;
            const std::vector<injector::post_op_type> accepted_post_ops
                    = {injector::eltwise, injector::binary};
            const memory_desc_wrapper dst_d(dst_md());
            injector::post_ops_ok_args_t post_ops_args(get_max_cpu_isa(),
                    accepted_post_ops, attr()->post_ops_, &dst_d, true, true,
                    true, softmax_impl::get_supported_bcast_strategies());
            return !with_sum && injector::post_ops_ok(post_ops_args);
        }
    };

    jit_uni_softmax_fwd_t(const pd_t *apd);
    ~jit_uni_softmax_fwd_t();

    status_t init(engine_t *engine) override;

    status_t execute(const exec_ctx_t &ctx) const override;

private:
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    softmax_impl::driver_t *softmax_driver_;
};

struct jit_uni_softmax_bwd_t : public primitive_t {
    struct pd_t : public cpu_softmax_bwd_pd_t {
        using cpu_softmax_bwd_pd_t::cpu_softmax_bwd_pd_t;

        DECLARE_COMMON_PD_T("jit:uni", jit_uni_softmax_bwd_t);

        status_t init(engine_t *engine) {
            auto is_dense = [&]() {
                const memory_desc_wrapper dst_d(dst_md());
                const auto &bd = dst_d.blocking_desc();

                if (!dst_d.is_dense(true) || !dst_d.only_padded_dim(axis()))
                    return false;

                // It is fine to use float here as the kernel uses halfs of
                // vector registers.
                const dim_t blk_size
                        = isa_max_vlen(get_max_cpu_isa()) / sizeof(float);
                if (dst_d.is_plain())
                    return bd.strides[axis()] == 1;
                else {
                    // 31 is a general limit, 2 is for unroll_regs_ = 4;
                    const size_t max_stride = (1LL << (31 - 2)) - 1;
                    const int last_blk = bd.inner_nblks - 1;
                    return bd.inner_blks[last_blk] == blk_size
                            && bd.inner_idxs[last_blk] == axis()
                            && sizeof(float) * bd.strides[axis()] < max_stride;
                }
            };

            using namespace data_type;
            bool ok = !is_fwd() && !has_zero_dim_memory()
                    && utils::one_of(dst_md()->data_type, f32, bf16, f16)
                    && utils::one_of(diff_dst_md()->data_type, f32, bf16, f16)
                    && utils::one_of(diff_src_md()->data_type, f32, bf16, f16)
                    && IMPLICATION(utils::one_of(bf16, dst_md()->data_type,
                                           diff_dst_md()->data_type,
                                           diff_src_md()->data_type),
                            mayiuse(avx512_core))
                    && IMPLICATION(utils::one_of(f16, dst_md()->data_type,
                                           diff_dst_md()->data_type,
                                           diff_src_md()->data_type),
                            mayiuse(avx512_core_fp16))
                    && attr()->has_default_values()
                    && set_default_formats() == status::success;
            if (!ok) return status::unimplemented;

            ok = memory_desc_wrapper(diff_src_md())
                            .similar_to(memory_desc_wrapper(diff_dst_md()),
                                    true, false, 0)
                    && memory_desc_wrapper(diff_dst_md())
                            == memory_desc_wrapper(dst_md())
                    && is_dense(); // not dense impl can be easily done
            if (!ok) return status::unimplemented;

            return status::success;
        };
    };

    jit_uni_softmax_bwd_t(const pd_t *apd);
    ~jit_uni_softmax_bwd_t();

    status_t init(engine_t *engine) override;

    status_t execute(const exec_ctx_t &ctx) const override;

private:
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }

    softmax_impl::driver_t *softmax_driver_;
};

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
