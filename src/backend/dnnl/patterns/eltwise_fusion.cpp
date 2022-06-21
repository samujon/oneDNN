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

#include "backend/dnnl/patterns/fusions.hpp"
#include "backend/dnnl/patterns/transformation_pattern.hpp"

#include "utils/pm/pbuilder.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {
namespace pattern {

namespace pm = impl::utils::pm;

using in_edges_t = pm::in_edges_t;
using pb_graph_t = pm::pb_graph_t;
using FCreateV2FusedOp = impl::pass::FCreateV2FusedOp;
using FCreateV2Pattern = impl::pass::FCreateV2Pattern;

/*!
 * \brief This provides eltwise-related fusion, i.e.
 *        relu-add fusion
 *        The process includes follow steps:
 *          1. look for fusion pattern on the graph
 *          2. If found, verify if this transformation is safe / correct
 *          3. replace the pattern with a fused op, update the graph
 */

DNNL_BACKEND_REGISTER_PATTERN_DEF_BEGIN(eltwise_fusion)

DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, eltwise_binary_fusion)
        .set_priority(8.2f)
        .set_kind(impl::partition_kind::unary_post_ops)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    pm::pb_op_t *peltwise = pgraph->append_alternation(
                            {impl::op_kind::Abs, impl::op_kind::Clamp,
                                    impl::op_kind::Elu, impl::op_kind::Exp,
                                    impl::op_kind::GELU,
                                    impl::op_kind::HardSwish,
                                    impl::op_kind::LeakyReLU,
                                    impl::op_kind::Log, impl::op_kind::Sigmoid,
                                    impl::op_kind::SoftPlus, impl::op_kind::Pow,
                                    impl::op_kind::ReLU, impl::op_kind::Round,
                                    impl::op_kind::Sqrt, impl::op_kind::Square,
                                    impl::op_kind::Tanh},
                            "peltwise");
                    auto pbinary_graph
                            = std::make_shared<pb_graph_t>("pbinary_graph");
                    pm::pb_op_t *pbinary_op = pbinary_graph->append_alternation(
                            {impl::op_kind::Add, impl::op_kind::Multiply,
                                    impl::op_kind::Maximum,
                                    impl::op_kind::Minimum,
                                    impl::op_kind::Divide,
                                    impl::op_kind::Subtract},
                            "pbinary_op");
                    pbinary_graph->create_input_port(0, pbinary_op, 0);
                    pbinary_graph->create_input_port(1, pbinary_op, 1);
                    pbinary_graph->create_output_port(0, pbinary_op, 0);

                    pgraph->append_repetition(pbinary_graph, {0, 0}, 1,
                            MAX_REPETITION,
                            in_edges_t {in_edge(0, peltwise, 0)},
                            "prepetition");
                })
        .set_attr<FCreateV2FusedOp>(
                "FCreateV2FusedOp", []() -> std::shared_ptr<op_t> {
                    std::shared_ptr<op_t> fused_op
                            = std::make_shared<op_t>(op_kind::eltwise_binary);
                    fused_op->set_attr<std::string>(op_attr::backend, "dnnl");
                    return fused_op;
                });

DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, chained_relu_fusion)
        .set_priority(5.0f)
        .set_kind(impl::partition_kind::unary_post_ops)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto chained_relu = std::make_shared<pb_graph_t>();
                    pm::pb_op_t *relu
                            = chained_relu->append_op(impl::op_kind::ReLU);
                    chained_relu->create_input_port(0, relu, 0);
                    chained_relu->create_output_port(0, relu, 0);

                    pgraph->append_repetition(
                            chained_relu, {0, 0}, 1, MAX_REPETITION);
                })
        .set_attr<FCreateV2FusedOp>(
                "FCreateV2FusedOp", []() -> std::shared_ptr<op_t> {
                    std::shared_ptr<op_t> fused_op
                            = std::make_shared<op_t>(op_kind::large_partition);
                    fused_op->set_attr<std::string>(op_attr::backend, "dnnl");
                    return fused_op;
                });

DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, int8_relu_fusion)
        .set_priority(9.9f)
        .set_kind(impl::partition_kind::quantized_unary_post_ops)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto dequant_data
                            = pgraph->append_op(impl::op_kind::Dequantize);
                    auto relu = pgraph->append_op(
                            impl::op_kind::ReLU, {in_edge(0, dequant_data, 0)});
                    pgraph->append_op(
                            impl::op_kind::Quantize, {in_edge(0, relu, 0)});
                })
        .set_attr<FCreateV2FusedOp>(
                "FCreateV2FusedOp", []() -> std::shared_ptr<op_t> {
                    auto fused_op = std::make_shared<op_t>(op_kind::int8_relu);
                    fused_op->set_attr<std::string>(op_attr::backend, "dnnl");
                    return fused_op;
                });

DNNL_BACKEND_REGISTER_TRANSFORMATION_PATTERN(dnnl, int8_relu_add_fusion)
        .set_priority(10.0f)
        .set_kind(impl::partition_kind::quantized_unary_post_ops)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto dequant_data
                            = pgraph->append_op(impl::op_kind::Dequantize);
                    auto dequant_other
                            = pgraph->append_op(impl::op_kind::Dequantize);
                    auto relu = pgraph->append_op(
                            impl::op_kind::ReLU, {in_edge(0, dequant_data, 0)});
                    auto add = pgraph->append_op(impl::op_kind::Add,
                            in_edges_t {in_edge(0, relu, 0),
                                    in_edge(1, dequant_other, 0)});
                    pgraph->append_op(
                            impl::op_kind::Quantize, {in_edge(0, add, 0)});
                })
        .set_attr<FCreateV2FusedOp>(
                "FCreateV2FusedOp", []() -> std::shared_ptr<op_t> {
                    auto fused_op
                            = std::make_shared<op_t>(op_kind::int8_relu_add);
                    fused_op->set_attr<std::string>(op_attr::backend, "dnnl");
                    return fused_op;
                });

DNNL_BACKEND_REGISTER_PATTERN_DEF_END

} // namespace pattern
} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl
