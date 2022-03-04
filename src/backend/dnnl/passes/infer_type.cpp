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

#include "interface/c_types_map.hpp"
#include "interface/graph.hpp"
#include "interface/logical_tensor.hpp"

#include "backend/dnnl/internal_ops.hpp"
#include "backend/dnnl/passes/utils.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {
using op_t = impl::op_t;
using ltw = impl::logical_tensor_wrapper_t;

/// This function is used to infer dtype for the internal edges of a subgraph.
/// The dtype of entire subgraph's in/outputs should be given before infer type.
/// The workflow of infer shape pass is:
///     Step1: check if the entire subgraph's all in/outputs have valid dtype.
///     Step2: visit each op in topological order, infer each op's inputs or
///     outputs type
/// \note
/// The infer type for each op should be bidirectional to support both inferring
/// outputs dtype according to inputs and inferring inputs dtype according to
/// outputs. Because inferring type for some ops is impossible. We have to skip
/// these ops and their outputs dtype should be determined by the consumers.
/// Take the following transformed INT8_Conv pattern as example:
///     (u8) \  / (s8)
///        dnnl_conv
///            | (unknown)
///         convert
///            | (u8)
/// Users specify the pattern's inputs to be u8/s8 and the outputs to be u8;
/// According to the u8/s8 inputs, we can't deduce dnnl_conv's output dtype; We
/// have to deduce it according to convert op's output dtype.
impl::status_t infer_type(std::shared_ptr<subgraph_t> &sg) {
    // Check inputs dtype
    for (impl::value_t *in : sg->get_input_values()) {
        impl::logical_tensor_t lt = in->get_logical_tensor();
        if (ltw(lt).data_type() == impl::data_type::undef)
            return impl::status::invalid_type;
    }

    // Check outputs dtype
    for (impl::value_t *out : sg->get_output_values()) {
        impl::logical_tensor_t lt = out->get_logical_tensor();
        if (ltw(lt).data_type() == impl::data_type::undef)
            return impl::status::invalid_type;
    }

    bool changed;
    do {
        changed = false;
        impl::status_t ret;
        ret = impl::topo_order_visit(sg->get_output_ops(), [&](impl::op_t *op) {
            if (op->get_kind() == op_kind::dnnl_mul_scales
                    || op->get_kind() == op_kind::dnnl_constant_scales) {
                auto out_lt = op->get_output_value(0)->get_logical_tensor();
                if (out_lt.data_type == impl::data_type::undef) {
                    op->get_output_value(0)->set_data_type(
                            impl::data_type::f32);
                    changed = changed || true;
                }
            } else if (op->get_kind() == op_kind::dnnl_constant_zps) {
                auto out_lt = op->get_output_value(0)->get_logical_tensor();
                if (out_lt.data_type == impl::data_type::undef) {
                    op->get_output_value(0)->set_data_type(
                            impl::data_type::s32);
                    changed = changed || true;
                }
            } else if (op->get_kind() == op_kind::permute
                    || op->get_kind() == op_kind::dnnl_reorder
                    || op->get_kind() == op_kind::to_group
                    || op->get_kind() == op_kind::expand
                    || op->get_kind() == op_kind::squeeze
                    || op->get_kind() == impl::op_kind::StaticReshape
                    || op->get_kind() == op_kind::dnnl_binary
                    || op->get_kind() == op_kind::dnnl_eltwise
                    || op->get_kind() == op_kind::dnnl_softmax
                    || op->get_kind() == op_kind::dnnl_logsoftmax
                    || op->get_kind() == impl::op_kind::StaticTranspose) {
                auto in_lt = op->get_input_value(0)->get_logical_tensor();
                auto out_lt = op->get_output_value(0)->get_logical_tensor();
                if (out_lt.data_type == impl::data_type::undef
                        && out_lt.data_type != in_lt.data_type) {
                    op->get_output_value(0)->set_data_type(in_lt.data_type);
                    changed = changed || true;
                } else if (in_lt.data_type == impl::data_type::undef
                        && in_lt.data_type != out_lt.data_type) {
                    op->get_input_value(0)->set_data_type(out_lt.data_type);
                    changed = changed || true;
                }
            } else if (op->get_kind() == op_kind::dnnl_bn_folding) {
                // skip the scratchpad
                for (size_t i = 0; i < op->num_outputs() - 1; i++) {
                    auto in_lt = op->get_input_value(i)->get_logical_tensor();
                    auto out_lt = op->get_output_value(i)->get_logical_tensor();
                    if (out_lt.data_type == impl::data_type::undef) {
                        op->get_output_value(i)->set_data_type(in_lt.data_type);
                    } else {
                        op->get_input_value(i)->set_data_type(out_lt.data_type);
                    }
                }
            } else {
                // some ops output type can't be inferred, it only can be
                // specified. so skip these ops (such as, dnnl_convolution,
                // dnnl_pool)
            }
            return impl::status::success;
        });

        if (ret != impl::status::success) return ret;
    } while (changed);

    return impl::status::success;
}

} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl
