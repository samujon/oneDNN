/*******************************************************************************
* Copyright 2020 Intel Corporation
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
#include "cpu/x64/prelu/jit_uni_prelu_backward_kernel.hpp"
#include <type_traits>

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

jit_prelu_backward_kernel_t::jit_prelu_backward_kernel_t(
        const cpu_prelu_bwd_pd_t *pd, int vlen)
    : pd_(pd)
    , simd_w_(vlen / sizeof(float))
    , bcast_(prelu::get_bcast_type(memory_desc_wrapper(pd_->diff_src_md()),
              memory_desc_wrapper(pd_->diff_weights_md())))
    , tail_size_(calc_tail_size())
    , data_type_(pd_->src_md()->data_type) {}

size_t jit_prelu_backward_kernel_t::simd_w() const noexcept {
    return simd_w_;
}

prelu::bcast jit_prelu_backward_kernel_t::get_bcast() const noexcept {
    return bcast_;
}

size_t jit_prelu_backward_kernel_t::calc_tail_size() const noexcept {

    const auto src_diff_d = memory_desc_wrapper(pd_->diff_src_md());
    dim_t nelems = 0;
    if (bcast_ == prelu::bcast::full) nelems = src_diff_d.nelems();

    return nelems % simd_w_;
}

void jit_prelu_backward_kernel_t::generate() {
    Xbyak::Label unroll_loop, unroll_loop_tail, nelems_tail, end;
    const auto dt_size = types::data_type_size(data_type_);
    const auto dt_vec_size = simd_w_ * dt_size;
    const auto unrolling_factor = get_unrolling_factor();
    preamble();
    load_kernel_call_params();
    prepare_kernel_const_vars();

    xor_(reg_offset_, reg_offset_);
    L(unroll_loop);
    {
        const size_t offt = unrolling_factor * dt_vec_size;
        cmp(reg_data_size_, offt);
        jl(unroll_loop_tail, T_NEAR);

        compute_dst(unrolling_factor, false /*tail*/);
        sub(reg_data_size_, offt);
        add(reg_offset_, offt);
        jmp(unroll_loop);
    }

    static constexpr size_t single_unrolling = 1u;
    L(unroll_loop_tail);
    {
        cmp(reg_data_size_, dt_vec_size);
        jl(nelems_tail, T_NEAR);

        compute_dst(single_unrolling, false /*tail*/);
        sub(reg_data_size_, dt_vec_size);
        add(reg_offset_, dt_vec_size);
        jmp(unroll_loop_tail);
    }

    L(nelems_tail);
    {
        cmp(reg_data_size_, 1);
        jl(end, T_NEAR);

        compute_dst(single_unrolling, true /*tail*/);
    }

    L(end);

    postamble();
}

#define PARAM_OFF(x) offsetof(call_params_t, x)

void jit_prelu_backward_kernel_t::load_kernel_call_params() {
    mov(reg_src_, ptr[abi_param1 + PARAM_OFF(src)]);
    mov(reg_weights_, ptr[abi_param1 + PARAM_OFF(weights)]);
    mov(reg_src_diff_, ptr[abi_param1 + PARAM_OFF(src_diff)]);
    mov(reg_weights_diff_, ptr[abi_param1 + PARAM_OFF(weights_diff)]);
    mov(reg_dst_diff_, ptr[abi_param1 + PARAM_OFF(dst_diff)]);
    mov(reg_data_size_, ptr[abi_param1 + PARAM_OFF(compute_data_size)]);
}

#undef PARAM_OFF

Xbyak::Address jit_prelu_backward_kernel_t::data_ptr(int arg_num, size_t offt) {
    switch (arg_num) {
        case DNNL_ARG_SRC: return ptr[reg_src_ + reg_offset_ + offt];
        case DNNL_ARG_WEIGHTS: return ptr[reg_weights_ + reg_offset_ + offt];
        case DNNL_ARG_DIFF_SRC: return ptr[reg_src_diff_ + reg_offset_ + offt];
        case DNNL_ARG_DIFF_WEIGHTS:
            return ptr[reg_weights_diff_ + reg_offset_ + offt];
        case DNNL_ARG_DIFF_DST: return ptr[reg_dst_diff_ + reg_offset_ + offt];

        default: assert(!"unsupported arg_num"); break;
    }
    return Xbyak::Address(0);
}

template <typename Vmm>
jit_uni_prelu_backward_kernel_t<Vmm>::jit_uni_prelu_backward_kernel_t(
        const cpu_prelu_bwd_pd_t *pd, const cpu_isa_t &isa)
    : jit_prelu_backward_kernel_t(pd, prelu::get_vlen(isa))
    , isa_(isa)
    , number_vmms_reserved_const_vars_(0)
    , vmm_zeros_(reserve_vmm())
    , tail_vmm_mask_(tail_size_ && utils::one_of(isa, avx, avx2) ? reserve_vmm()
                                                                 : Vmm(0))
    , vmm_ones_(reserve_vmm())
    , unrolling_factor_(calc_unrolling_factor())
    , io_(this, isa, data_type_, tail_size_, tail_opmask_, tail_vmm_mask_,
              reg_tmp_) {}

template <typename Vmm>
jit_uni_prelu_backward_kernel_t<Vmm>::~jit_uni_prelu_backward_kernel_t()
        = default;

template <typename Vmm>
void jit_uni_prelu_backward_kernel_t<Vmm>::prepare_kernel_const_vars() {
    uni_vxorps(vmm_zeros_, vmm_zeros_, vmm_zeros_);
    if (tail_size_) io_.prepare_tail_mask();
    this->mov(this->reg_tmp_, float2int(1));
    const Xbyak::Xmm xmm_ones_ {vmm_ones_.getIdx()};
    this->uni_vmovq(xmm_ones_, this->reg_tmp_);
    this->uni_vbroadcastss(vmm_ones_, xmm_ones_);
}

template <typename Vmm>
size_t jit_uni_prelu_backward_kernel_t<Vmm>::get_unrolling_factor() const {
    return unrolling_factor_;
}

template <typename Vmm>
Vmm jit_uni_prelu_backward_kernel_t<Vmm>::reserve_vmm() {
    return Vmm(number_vmms_reserved_const_vars_++);
}

template <typename Vmm>
size_t jit_uni_prelu_backward_kernel_t<Vmm>::get_number_reserved_vmms() const
        noexcept {
    return number_vmms_reserved_const_vars_;
}

template <>
size_t
jit_uni_prelu_backward_kernel_t<Xbyak::Zmm>::get_number_reserved_vmms() const
        noexcept {
    static constexpr size_t number_vmm_reserved_bf16_process = 4u;
    const bool process_bf16_with_emu
            = data_type_ == data_type::bf16 && isa_ == avx512_core;

    return number_vmms_reserved_const_vars_
            + (process_bf16_with_emu ? number_vmm_reserved_bf16_process : 0);
}

template <typename Vmm>
size_t jit_uni_prelu_backward_kernel_t<Vmm>::calc_unrolling_factor() const
        noexcept {
    const auto n_vregs = prelu::get_n_vregs(isa_);
    const size_t number_of_available_regs
            = n_vregs - get_number_reserved_vmms();
    const size_t max_unrolling_factor
            = number_of_available_regs / number_vmm_single_compute_;

    const auto diff_src_d = memory_desc_wrapper(pd_->diff_src_md());
    size_t single_thread_estimated_elems = 0;

    if (bcast_ == prelu::bcast::full) {
        const size_t nelems = diff_src_d.nelems();
        single_thread_estimated_elems = nelems / dnnl_get_max_threads();
    }

    const size_t estimated_vectors_used = nstl::max(
            static_cast<size_t>(
                    std::floor(single_thread_estimated_elems / simd_w_)),
            static_cast<size_t>(1));

    return nstl::min(max_unrolling_factor, estimated_vectors_used);
}

template <typename Vmm>
void jit_uni_prelu_backward_kernel_t<Vmm>::compute_dst(
        int unrolling_factor, int tail) {

    static constexpr size_t dst_diff_idx = 0;
    static constexpr size_t src_idx = 1;
    static constexpr size_t src_le_zero_idx = 2;
    static constexpr size_t src_gt_zero_idx = 3;
    static constexpr size_t weights_diff_idx = 4;
    static constexpr size_t weights_idx = 5;
    const auto dt_size = types::data_type_size(data_type_);

    for (size_t unroll_group = 0; unroll_group < unrolling_factor;
            ++unroll_group) {

        const Vmm dst_diff_vmm = get_compute_vmm(dst_diff_idx, unroll_group);
        const Vmm src_vmm = get_compute_vmm(src_idx, unroll_group);
        const Vmm src_le_zero_vmm
                = get_compute_vmm(src_le_zero_idx, unroll_group);
        const Vmm src_gt_zero_vmm
                = get_compute_vmm(src_gt_zero_idx, unroll_group);
        const Vmm weights_diff_vmm
                = get_compute_vmm(weights_diff_idx, unroll_group);
        const Vmm weights_vmm = get_compute_vmm(weights_idx, unroll_group);

        const auto offset = unroll_group * simd_w_ * dt_size;
        io_.load(data_ptr(DNNL_ARG_DIFF_DST, offset), dst_diff_vmm, tail);
        io_.load(data_ptr(DNNL_ARG_SRC, offset), src_vmm, tail);
        static constexpr int VCMPLEPS = 2;
        uni_vcmpps(src_le_zero_vmm, src_vmm, vmm_zeros_, VCMPLEPS);
        uni_vandps(src_le_zero_vmm, src_le_zero_vmm, vmm_ones_);
        static constexpr int VCMPGTPS = 14;
        uni_vcmpps(src_gt_zero_vmm, src_vmm, vmm_zeros_, VCMPGTPS);
        uni_vandps(src_gt_zero_vmm, src_gt_zero_vmm, vmm_ones_);

        //weights_diff_calculations
        uni_vmulps(weights_diff_vmm, dst_diff_vmm, src_vmm);
        uni_vmulps(weights_diff_vmm, weights_diff_vmm, src_le_zero_vmm);
        io_.store(weights_diff_vmm, data_ptr(DNNL_ARG_DIFF_WEIGHTS, offset),
                tail);

        //src_diff calculations
        io_.load(data_ptr(DNNL_ARG_WEIGHTS, offset), weights_vmm, tail);
        uni_vfmadd231ps(src_gt_zero_vmm, src_le_zero_vmm, weights_vmm);
        const auto &src_diff_vmm = src_gt_zero_vmm;
        uni_vmulps(src_diff_vmm, src_diff_vmm, dst_diff_vmm);
        io_.store(src_diff_vmm, data_ptr(DNNL_ARG_DIFF_SRC, offset), tail);
    }
}

template <>
void jit_uni_prelu_backward_kernel_t<Xbyak::Zmm>::compute_dst(
        int unrolling_factor, int tail) {

    size_t opmask_counter = 2;
    auto get_next_opmask = [opmask_counter]() mutable {
        static constexpr size_t opmask_range_begin = 2;
        static constexpr size_t opmask_range_end = 8;
        const auto opmask = Xbyak::Opmask(opmask_counter++);
        if (opmask_counter == opmask_range_end)
            opmask_counter = opmask_range_begin;
        return opmask;
    };

    static constexpr size_t dst_diff_idx = 0;
    static constexpr size_t src_idx = 1;
    static constexpr size_t weights_diff_idx = 2;
    static constexpr size_t weights_idx = 4;

    const auto dt_size = types::data_type_size(data_type_);

    for (size_t unroll_group = 0; unroll_group < unrolling_factor;
            ++unroll_group) {

        const auto offset = unroll_group * simd_w_ * dt_size;
        const Xbyak::Zmm dst_diff_vmm
                = get_compute_vmm(dst_diff_idx, unroll_group);
        const Xbyak::Zmm src_vmm = get_compute_vmm(src_idx, unroll_group);

        io_.load(data_ptr(DNNL_ARG_DIFF_DST, offset), dst_diff_vmm, tail);
        io_.load(data_ptr(DNNL_ARG_SRC, offset), src_vmm, tail);

        const Xbyak::Opmask src_le_zero_opmask = get_next_opmask();
        static constexpr int VCMPLEPS = 2;
        vcmpps(src_le_zero_opmask, src_vmm, vmm_zeros_, VCMPLEPS);
        const Xbyak::Opmask src_gt_zero_vmm_opmask = get_next_opmask();
        static constexpr int VCMPGTPS = 14;
        vcmpps(src_gt_zero_vmm_opmask, src_vmm, vmm_zeros_, VCMPGTPS);

        //weights_diff_calculations
        const Xbyak::Zmm weights_diff_vmm
                = get_compute_vmm(weights_diff_idx, unroll_group);
        vmulps(weights_diff_vmm | src_le_zero_opmask | T_z, dst_diff_vmm,
                src_vmm);
        io_.store(weights_diff_vmm, data_ptr(DNNL_ARG_DIFF_WEIGHTS, offset),
                tail);

        //src_diff calculations
        const Xbyak::Zmm weights_vmm
                = get_compute_vmm(weights_idx, unroll_group);
        const auto &src_diff_vmm = weights_vmm;
        io_.load(data_ptr(DNNL_ARG_WEIGHTS, offset), weights_vmm, tail);
        vmovaps(src_diff_vmm | src_le_zero_opmask | T_z, weights_vmm);
        vaddps(src_diff_vmm | src_gt_zero_vmm_opmask, src_diff_vmm, vmm_ones_);
        vmulps(src_diff_vmm, src_diff_vmm, dst_diff_vmm);
        io_.store(src_diff_vmm, data_ptr(DNNL_ARG_DIFF_SRC, offset), tail);
    }
}

template <typename Vmm>
Vmm jit_uni_prelu_backward_kernel_t<Vmm>::get_compute_vmm(
        size_t base_idx, size_t unroll_group) {
    return Vmm(number_vmms_reserved_const_vars_ + base_idx
            + unroll_group * number_vmm_single_compute_);
}

jit_prelu_backward_kernel_t *jit_prelu_backward_kernel_t::create(
        const cpu_prelu_bwd_pd_t *pd) {

    const auto isa = prelu::get_supported_isa();

    if (utils::one_of(isa, avx512_core_bf16, avx512_core, avx512_common))
        return new jit_uni_prelu_backward_kernel_t<Xbyak::Zmm>(pd, isa);
    else if (utils::one_of(isa, avx, avx2))
        return new jit_uni_prelu_backward_kernel_t<Xbyak::Ymm>(pd, isa);
    else if (isa == sse41)
        return new jit_uni_prelu_backward_kernel_t<Xbyak::Xmm>(pd, isa);

    return nullptr;
}

template class jit_uni_prelu_backward_kernel_t<Xbyak::Zmm>;
template class jit_uni_prelu_backward_kernel_t<Xbyak::Ymm>;
template class jit_uni_prelu_backward_kernel_t<Xbyak::Xmm>;

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl
