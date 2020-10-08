/*******************************************************************************
* Copyright 2020 Arm Ltd. and affiliates
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

#include "dnnl_types.h"

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"
#include "cpu/aarch64/acl_gemm_convolution.hpp"

#include <cstring>

namespace dnnl {
namespace impl {
namespace cpu {
namespace aarch64 {

using namespace dnnl::impl::status;
using namespace dnnl::impl::memory_tracking::names;
using namespace dnnl::impl::utils;

template <data_type_t src_type, data_type_t wei_type, data_type_t dst_type,
        data_type_t bia_type>
status_t acl_gemm_convolution_fwd_t<src_type, wei_type, dst_type,
        bia_type>::execute_forward(const exec_ctx_t &ctx) const {
    status_t status = status::success;
    auto src_base = CTX_IN_MEM(const src_data_t *, DNNL_ARG_SRC);
    auto wei_base = CTX_IN_MEM(const wei_data_t *, DNNL_ARG_WEIGHTS);
    auto bia_base = CTX_IN_MEM(const bia_data_t *, DNNL_ARG_BIAS);
    auto dst_base = CTX_OUT_MEM(dst_data_t *, DNNL_ARG_DST);

    bool with_bias = pd()->acp_.with_bias;

    // Retrieve primitive resource and configured Compute Library objects
    auto *acl_resource = ctx.get_resource_mapper()->get<acl_resource_t>(this);
    acl_obj_t<arm_compute::NEGEMMConvolutionLayer> &acl_obj
            = acl_resource->get_acl_obj();

    acl_obj.src_tensor.allocator()->import_memory(
            const_cast<src_data_t *>(src_base));
    acl_obj.wei_tensor.allocator()->import_memory(
            const_cast<wei_data_t *>(wei_base));
    acl_obj.dst_tensor.allocator()->import_memory(dst_base);

    // Retrieve extra bias memory from the scratchpad and copy from user memory
    if (with_bias) {
        const auto scratchpad = ctx.get_scratchpad_grantor();
        auto *bia_memory = scratchpad.template get<bia_data_t>(
                memory_tracking::names::key_none);
        size_t oc = acl_obj.bia_tensor.info()->tensor_shape()[0];
        std::memcpy(bia_memory, bia_base, oc * sizeof(bia_data_t));
        acl_obj.bia_tensor.allocator()->import_memory(bia_memory);
    }

    acl_obj.conv.run();

    acl_obj.src_tensor.allocator()->free();
    acl_obj.wei_tensor.allocator()->free();
    acl_obj.dst_tensor.allocator()->free();
    if (with_bias) { acl_obj.bia_tensor.allocator()->free(); }

    return status;
}

using namespace data_type;
template struct acl_gemm_convolution_fwd_t<f32>;
template struct acl_gemm_convolution_fwd_t<s8, s8, s8, s32>;

} // namespace aarch64
} // namespace cpu
} // namespace impl
} // namespace dnnl
