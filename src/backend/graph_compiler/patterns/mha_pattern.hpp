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
#ifndef BACKEND_GRAPH_COMPILER_PATTERNS_MHA_PATTERN_HPP
#define BACKEND_GRAPH_COMPILER_PATTERNS_MHA_PATTERN_HPP

#include <memory>

#include "backend/graph_compiler/patterns/transformation_pattern.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace compiler_impl {
namespace pass {

using pb_graph_t = impl::utils::pm::pb_graph_t;
using FCreateV2Pattern = impl::pass::FCreateV2Pattern;

template <impl::data_type_t DTYPE>
bool check_input_dtype(op_t *op) {
    for (size_t i = 0; i < op->num_inputs(); ++i) {
        logical_tensor_t iport = op->get_input_value(i)->get_logical_tensor();
        if (iport.data_type != DTYPE) return false;
    }
    return true;
}

COMPILER_BACKEND_REGISTER_PASSES_DEF_BEGIN(fp32_mha_pattern)
// fp32 MHA pattern
/*
                    [Key](f32)
                      |
   (f32)[Query]    Reshape
            |         |
        Reshape   Transpose
            |         |
        Transpose Transpose
              \     /
               MatMul  [fscore scale](f32)
                 \    /
[Attention Mask] Div|Mul  [Value](f32)
              \   /        |
                Add     Reshape
                 |         |
              Softmax  Transpose
                    \     /
                     MatMul
                        |
                    Transpose
                        |
                  Reshape (optional)
                        |
                     [output](f32)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(compiler, fp32_mha_pattern)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto query_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape, "query_reshape");
                    query_reshape->append_decision_function(
                            check_input_dtype<impl::data_type::f32>);
                    auto query_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, query_reshape, 0)}, "query_transpose");
                    query_transpose->append_decision_function(
                            check_input_dtype<impl::data_type::f32>);
                    auto key_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape, "key_reshape");
                    auto key_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, key_reshape, 0)}, "key_transpose");
                    auto key_transpose2 = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, key_transpose, 0)}, "key_transpose2");
                    auto matmul_qk = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, query_transpose, 0),
                                    in_edge(1, key_transpose2, 0)},
                            "matmul_qk");
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto value_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape, "value_reshape");
                    auto value_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, value_reshape, 0)}, "value_transpose");
                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, softmax, 0),
                                    in_edge(1, value_transpose, 0)},
                            "matmul_v");
                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");

                    auto optional_reshape_subgraph
                            = std::make_shared<pb_graph_t>(
                                    "optional_reshape_subgraph");
                    auto optional_reshape
                            = optional_reshape_subgraph->append_op(
                                    impl::op_kind::StaticReshape,
                                    "optional_reshape");
                    optional_reshape_subgraph->create_input_port(
                            0, optional_reshape, 0);
                    optional_reshape_subgraph->create_output_port(
                            0, optional_reshape, 0);

                    pgraph->append_optional(optional_reshape_subgraph,
                            {in_edge(0, transpose_output, 0)},
                            "reshape_output");
                });

// fp32 MHA pattern alternative
/*
   (f32)[Query]    [Key](f32)
              \     /
               MatMul  [fscore scale](f32)
                 \    /
[Attention Mask] Div|Mul
              \   /
                Add
                 |
              Softmax  [Value](f32)
                    \     /
                     MatMul
                        |
                    Transpose
                        |
                     Reorder
                        |
                     [output](f32)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(
        compiler, fp32_mha_pattern_alternative)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto matmul_qk = pgraph->append_op(
                            impl::op_kind::MatMul, "matmul_qk");
                    matmul_qk->append_decision_function(
                            check_input_dtype<impl::data_type::f32>);
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, softmax, 0)}, "matmul_v");
                    matmul_v->append_decision_function(
                            check_input_dtype<impl::data_type::f32>);
                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");
                    pgraph->append_op(impl::op_kind::Reorder,
                            {in_edge(0, transpose_output, 0)},
                            "reorder_output");
                });
COMPILER_BACKEND_REGISTER_PASSES_DEF_END

COMPILER_BACKEND_REGISTER_PASSES_DEF_BEGIN(bf16_mha_pattern)
// bf16 MHA pattern (it is the same as fp32 pattern except dtype)
/*
                    [Key](bf16)
                      |
  (bf16)[Query]    Reshape
            |         |
        Reshape   Transpose
            |         |
        Transpose Transpose
              \     /
               MatMul  [fscore scale]
                 \    /
[Attention Mask] Div|Mul  [Value](bf16)
              \   /        |
                Add     Reshape
                 |         |
              Softmax  Transpose
                    \     /
                     MatMul
                        |
                    Transpose
                        |
                  Reshape (optional)
                        |
                     [output](bf16)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(compiler, bf16_mha_pattern)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto query_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape, "query_reshape");
                    query_reshape->append_decision_function(
                            check_input_dtype<impl::data_type::bf16>);
                    auto query_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, query_reshape, 0)}, "query_transpose");
                    query_transpose->append_decision_function(
                            check_input_dtype<impl::data_type::bf16>);
                    auto key_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape, "key_reshape");
                    auto key_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, key_reshape, 0)}, "key_transpose");
                    auto key_transpose2 = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, key_transpose, 0)}, "key_transpose2");
                    auto matmul_qk = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, query_transpose, 0),
                                    in_edge(1, key_transpose2, 0)},
                            "matmul_qk");
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto value_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape, "value_reshape");
                    auto value_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, value_reshape, 0)}, "value_transpose");
                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, softmax, 0),
                                    in_edge(1, value_transpose, 0)},
                            "matmul_v");
                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");

                    auto optional_reshape_subgraph
                            = std::make_shared<pb_graph_t>(
                                    "optional_reshape_subgraph");
                    auto optional_reshape
                            = optional_reshape_subgraph->append_op(
                                    impl::op_kind::StaticReshape,
                                    "optional_reshape");
                    optional_reshape_subgraph->create_input_port(
                            0, optional_reshape, 0);
                    optional_reshape_subgraph->create_output_port(
                            0, optional_reshape, 0);

                    pgraph->append_optional(optional_reshape_subgraph,
                            {in_edge(0, transpose_output, 0)},
                            "reshape_output");
                });

// bf16 MHA pattern alternative
/*
  (bf16)[Query]    [Key](bf16)
              \     /
               MatMul  [fscore scale]
                 \    /
[Attention Mask] Div|Mul
              \   /
                Add
                 |
              Softmax  [Value](bf16)
                    \     /
                     MatMul
                        |
                    Transpose
                        |
                     Reorder
                        |
                     [output](bf16)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(
        compiler, bf16_mha_pattern_alternative)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto matmul_qk = pgraph->append_op(
                            impl::op_kind::MatMul, "matmul_qk");
                    matmul_qk->append_decision_function(
                            check_input_dtype<impl::data_type::bf16>);
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, softmax, 0)}, "matmul_v");
                    matmul_v->append_decision_function(
                            check_input_dtype<impl::data_type::bf16>);
                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");
                    pgraph->append_op(impl::op_kind::Reorder,
                            {in_edge(0, transpose_output, 0)},
                            "reorder_output");
                });
COMPILER_BACKEND_REGISTER_PASSES_DEF_END

COMPILER_BACKEND_REGISTER_PASSES_DEF_BEGIN(int8_mha_pattern)
// int8 MHA pattern
/*
                         [Key](u8/s8)
                           |
       (u8/s8)[Query]  Dequantize
                 |         |
             Dequantize Reshape
                 |         |
             Reshape   Transpose
                 |         |
             Transpose Transpose
                   \     /
                    MatMul  [Fscore Scale](f32)
                      \    /
(f32)[Attention Mask] Div|Mul
                   \   /
                     Add    [Value](u8/s8)
                      |         |
                   Softmax   Dequantize
                      |         |
                   Quantize   Reshape
                      |         |
                  Dequantize Transpose
                         \     /
                          MatMul
                             |
                         Transpose
                             |
                          Reshape (optional)
                             |
                          Quantize
                             |
                        [output](u8/s8)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(compiler, int8_mha_pattern)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto dequantize_query = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_query");
                    auto dequantize_key = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_key");
                    auto dequantize_value = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_value");
                    auto query_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape,
                            {in_edge(0, dequantize_query, 0)}, "query_reshape");
                    auto query_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, query_reshape, 0)}, "query_transpose");
                    auto key_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape,
                            {in_edge(0, dequantize_key, 0)}, "key_reshape");
                    auto key_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, key_reshape, 0)}, "key_transpose");
                    auto key_transpose2 = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, key_transpose, 0)}, "key_transpose2");
                    auto matmul_qk = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, query_transpose, 0),
                                    in_edge(1, key_transpose2, 0)},
                            "matmul_qk");
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto quantize_softmax = pgraph->append_op(
                            impl::op_kind::Quantize, {in_edge(0, softmax, 0)},
                            "quantize_softmax");
                    auto dequantize_softmax
                            = pgraph->append_op(impl::op_kind::Dequantize,
                                    {in_edge(0, quantize_softmax, 0)},
                                    "dequantize_softmax");
                    auto value_reshape = pgraph->append_op(
                            impl::op_kind::StaticReshape,
                            {in_edge(0, dequantize_value, 0)}, "value_reshape");
                    auto value_transpose = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, value_reshape, 0)}, "value_transpose");
                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, dequantize_softmax, 0),
                                    in_edge(1, value_transpose, 0)},
                            "matmul_v");

                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");

                    auto optional_reshape_subgraph
                            = std::make_shared<pb_graph_t>(
                                    "optional_reshape_subgraph");
                    auto optional_reshape
                            = optional_reshape_subgraph->append_op(
                                    impl::op_kind::StaticReshape,
                                    "optional_reshape");
                    optional_reshape_subgraph->create_input_port(
                            0, optional_reshape, 0);
                    optional_reshape_subgraph->create_output_port(
                            0, optional_reshape, 0);

                    auto reshape_output
                            = pgraph->append_optional(optional_reshape_subgraph,
                                    {in_edge(0, transpose_output, 0)},
                                    "reshape_output");
                    pgraph->append_op(impl::op_kind::Quantize,
                            {in_edge(0, reshape_output, 0)}, "quantize_output");
                });

/*
        (int8)[Query]   [Key](int8)
                 |          |
             Dequantize Dequantize
                   \     /
                    MatMul  [Fscore Scale](f32)
                      \    /
(f32)[Attention Mask] Div|Mul
                   \   /
                     Add
                      |
                   Softmax
                      |
                   Quantize  [Value](int8)
                      |          |
                  Dequantize Dequantize
                         \     /
                          MatMul
                             |
                         Transpose
                             |
                          Reorder
                             |
                          Quantize
                             |
                        [output](int8)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(
        compiler, int8_mha_pattern_alternative)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto dequantize_query = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_query");

                    auto dequantize_key = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_key");

                    auto matmul_qk = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, dequantize_query, 0),
                                    in_edge(1, dequantize_key, 0)},
                            "matmul_qk");
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto quantize_softmax = pgraph->append_op(
                            impl::op_kind::Quantize, {in_edge(0, softmax, 0)},
                            "quantize_softmax");
                    auto dequantize_softmax
                            = pgraph->append_op(impl::op_kind::Dequantize,
                                    {in_edge(0, quantize_softmax, 0)},
                                    "dequantize_softmax");

                    auto dequantize_value = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_value");
                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, dequantize_softmax, 0),
                                    in_edge(1, dequantize_value, 0)},
                            "matmul_v");

                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");
                    auto reorder_output
                            = pgraph->append_op(impl::op_kind::Reorder,
                                    {in_edge(0, transpose_output, 0)},
                                    "reorder_output");
                    pgraph->append_op(impl::op_kind::Quantize,
                            {in_edge(0, reorder_output, 0)}, "quantize_output");
                });

/*
        (int8)[Query]   [Key](int8)
                 |          |
             Dequantize Dequantize
                 |          |
              TypeCast   TypeCast
                   \     /
                    MatMul  [Fscore Scale](f32)
                      \    /
(bf16)[Attention Mask] Div|Mul
                   \   /
                     Add
                      |
                   Softmax
                      |
                   TypeCast
                      |
                   Quantize  [Value](int8)
                      |          |
                  Dequantize Dequantize
                      |          |
                   TypeCast   TypeCast
                         \     /
                          MatMul
                             |
                         Transpose
                             |
                          Reorder
                             |
                          TypeCast
                             |
                          Quantize
                             |
                        [output](int8)
*/
COMPILER_BACKEND_REGISTER_TRANSFORMATION_PASS(compiler, int8_bf16_mha_pattern)
        .set_priority(5.0f)
        .set_attr<FCreateV2Pattern>("FCreateV2Pattern",
                [](const std::shared_ptr<pb_graph_t> &pgraph) -> void {
                    auto dequantize_query = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_query");
                    auto cast_query = pgraph->append_op(impl::op_kind::TypeCast,
                            {in_edge(0, dequantize_query, 0)}, "cast_query");

                    auto dequantize_key = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_key");
                    auto cast_key = pgraph->append_op(impl::op_kind::TypeCast,
                            {in_edge(0, dequantize_key, 0)}, "cast_key");

                    auto matmul_qk = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, cast_query, 0),
                                    in_edge(1, cast_key, 0)},
                            "matmul_qk");
                    auto fscore_scale = pgraph->append_alternation(
                            {impl::op_kind::Divide, impl::op_kind::Multiply},
                            {in_edge(0, matmul_qk, 0)}, "fscore_scale");
                    auto fscore_add = pgraph->append_op(impl::op_kind::Add,
                            {in_edge(0, fscore_scale, 0)}, "fscore_add");
                    auto softmax = pgraph->append_op(impl::op_kind::SoftMax,
                            {in_edge(0, fscore_add, 0)}, "softmax");
                    auto cast_softmax_fp32 = pgraph->append_op(
                            impl::op_kind::TypeCast, {in_edge(0, softmax, 0)},
                            "cast_softmax_fp32");
                    auto quantize_softmax
                            = pgraph->append_op(impl::op_kind::Quantize,
                                    {in_edge(0, cast_softmax_fp32, 0)},
                                    "quantize_softmax");
                    auto dequantize_softmax
                            = pgraph->append_op(impl::op_kind::Dequantize,
                                    {in_edge(0, quantize_softmax, 0)},
                                    "dequantize_softmax");
                    auto cast_softmax
                            = pgraph->append_op(impl::op_kind::TypeCast,
                                    {in_edge(0, dequantize_softmax, 0)},
                                    "cast_softmax");

                    auto dequantize_value = pgraph->append_op(
                            impl::op_kind::Dequantize, "dequantize_value");
                    auto cast_value = pgraph->append_op(impl::op_kind::TypeCast,
                            {in_edge(0, dequantize_value, 0)}, "cast_value");

                    auto matmul_v = pgraph->append_op(impl::op_kind::MatMul,
                            {in_edge(0, cast_softmax, 0),
                                    in_edge(1, cast_value, 0)},
                            "matmul_v");
                    auto transpose_output = pgraph->append_op(
                            impl::op_kind::StaticTranspose,
                            {in_edge(0, matmul_v, 0)}, "transpose_output");
                    auto reorder_output
                            = pgraph->append_op(impl::op_kind::Reorder,
                                    {in_edge(0, transpose_output, 0)},
                                    "reorder_output");
                    auto cast_output_fp32
                            = pgraph->append_op(impl::op_kind::TypeCast,
                                    {in_edge(0, reorder_output, 0)},
                                    "cast_output_fp32");
                    pgraph->append_op(impl::op_kind::Quantize,
                            {in_edge(0, cast_output_fp32, 0)},
                            "quantize_output");
                });

COMPILER_BACKEND_REGISTER_PASSES_DEF_END

} // namespace pass
} // namespace compiler_impl
} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
