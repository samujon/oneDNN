/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
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

#ifndef GPU_JIT_CONV_KERNEL_BUILDER_HPP
#define GPU_JIT_CONV_KERNEL_BUILDER_HPP

#include <array>

#include "common/convolution_pd.hpp"
#include "gpu/jit/conv/config.hpp"
#include "gpu/jit/conv/gemm_schedule.hpp"
#include "gpu/jit/conv/ir.hpp"
#include "gpu/jit/conv/post_op_support.hpp"
#include "gpu/jit/conv/tensor.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

class kernel_builder_t {
public:
    kernel_builder_t(const conv_config_t &cfg, const convolution_pd_t *pd,
            const kernel_info_t &kernel_info)
        : cfg_(cfg), pd_(pd), kernel_info_(kernel_info) {
        build();
    }

    const stmt_t &stmt() const { return stmt_; }

    const grid_info_t &kernel_grid() const { return kernel_grid_; }

    const std::array<expr_t, 3> &local_id() const { return local_id_; }

private:
    void build();
    void init_fwd(gemm_schedule_t &gemm_schedule, view_t &src_view,
            view_t &wei_view, view_t &dst_view, expr_t &src_buf,
            expr_t &wei_buf, expr_t &dst_buf);
    void init_bwd_d(gemm_schedule_t &gemm_schedule, view_t &dst_view,
            view_t &wei_view, view_t &src_view, expr_t &dst_buf,
            expr_t &wei_buf, expr_t &src_buf);
    void init_bwd_w(gemm_schedule_t &gemm_schedule, view_t &src_view,
            view_t &dst_view, view_t &wei_view, view_t &bia_view,
            expr_t &src_buf, expr_t &dst_buf, expr_t &wei_buf, expr_t &bia_buf,
            expr_t &bia_reduction_condition);

    const conv_config_t &cfg_;
    const convolution_pd_t *pd_;
    const kernel_info_t &kernel_info_;

    std::array<expr_t, 3> local_id_; // Local IDs (OpenCL) for the 0-th lane.
    grid_info_t kernel_grid_; // Kernel grid (consisting of thread groups).
    grid_info_t tg_grid_; // Thread group grid (consisting of threads).

    stmt_t stmt_;
};

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
