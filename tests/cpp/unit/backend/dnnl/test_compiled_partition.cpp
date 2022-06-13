/*******************************************************************************
* Copyright 2020-2022 Intel Corporation
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

#include <gtest/gtest.h>

#include "interface/partition.hpp"
#include "interface/tensor.hpp"

#include "backend/dnnl/dnnl_partition_impl.hpp"

#include "cpp/unit/unit_test_common.hpp"
#include "cpp/unit/utils.hpp"

namespace utils = dnnl::graph::tests::unit::utils;

TEST(CompiledPartition, Relu) {
    impl::engine_t &eng = get_engine();

    impl::op_t relu_op(impl::op_kind::ReLU, "relu");

    const impl::logical_tensor_t lt_in = utils::logical_tensor_init(
            /* tid= */ 1, {1, 1, 3, 3}, impl::data_type::f32);
    const impl::logical_tensor_t lt_out
            = utils::logical_tensor_init(/* tid= */ 2, {1, 1, 3, 3},
                    impl::data_type::f32, impl::layout_type::any);

    relu_op.add_input(lt_in);
    relu_op.add_output(lt_out);

    impl::graph_t g(eng.kind());
    g.add_op(&relu_op);
    g.build_graph();

    auto pimpl = std::make_shared<impl::dnnl_impl::dnnl_partition_impl_t>(
            eng.kind(), impl::fpmath_mode::strict,
            impl::partition_kind::unary_post_ops);
    pimpl->add_op(std::make_shared<impl::op_t>(relu_op));
    pimpl->init(&relu_op);

    impl::partition_t p;
    p.init(pimpl);

    impl::compiled_partition_t cp(p);
    ASSERT_EQ(p.id(), cp.src_partition().id());

    std::vector<const impl::logical_tensor_t *> lt_inputs {&lt_in};
    std::vector<const impl::logical_tensor_t *> lt_outputs {&lt_out};
    impl::status_t status = p.compile(&cp, lt_inputs, lt_outputs, &eng);
    ASSERT_EQ(status, impl::status::success);
    impl::logical_tensor_t query_in_lt, query_out_lt;
    impl::status_t status_query
            = cp.query_logical_tensor(lt_out.id, &query_out_lt);
    ASSERT_EQ(status_query, impl::status::success);
    ASSERT_EQ(query_out_lt.layout_type, impl::layout_type::strided);

    size_t size_in = 0, size_out = 0;
    cp.query_logical_tensor(lt_in.id, &query_in_lt);
    size_in = impl::logical_tensor_wrapper_t(query_in_lt).size();
    size_out = impl::logical_tensor_wrapper_t(query_out_lt).size();
    ASSERT_EQ(size_in, 9 * sizeof(float));
    ASSERT_EQ(size_in, size_out);

    size_t ele_num_in = size_in / sizeof(float);
    test::vector<float> data_in(ele_num_in);
    test::vector<float> data_out(ele_num_in);
    for (size_t i = 0; i < ele_num_in; i++) {
        data_in[i] = static_cast<float>(i) - static_cast<float>(ele_num_in / 2);
    }

    impl::tensor_t t_in(lt_in, &eng, data_in.data()),
            t_out(query_out_lt, &eng, data_out.data());

    std::vector<impl::tensor_t> t_inputs, t_outputs;
    t_inputs.emplace_back(t_in);
    t_outputs.emplace_back(t_out);

    impl::stream_t &strm = get_stream();
    EXPECT_SUCCESS(cp.execute(&strm, t_inputs, t_outputs));
    strm.wait();

    std::unique_ptr<float[]> ref_out(new float[ele_num_in]);
    for (size_t i = 0; i < ele_num_in; i++) {
        ref_out[i] = (i < ele_num_in / 2)
                ? 0.0f
                : static_cast<float>(i) - static_cast<float>(ele_num_in / 2);
    }

    for (size_t i = 0; i < ele_num_in; i++) {
        ASSERT_FLOAT_EQ(ref_out[i], data_out[i]);
    }
}

TEST(CompiledPartition, SearchRequiredInputsOutputs) {
    impl::engine_t &eng = get_engine();

    impl::op_t relu_op(impl::op_kind::ReLU, "relu");

    impl::logical_tensor_t lt_in = utils::logical_tensor_init(
            /* tid= */ 1, {1, 1, 3, 3}, impl::data_type::f32);
    impl::logical_tensor_t lt_out = utils::logical_tensor_init(/* tid= */ 2,
            {1, 1, 3, 3}, impl::data_type::f32, impl::layout_type::any);

    relu_op.add_input(lt_in);
    relu_op.add_output(lt_out);

    impl::graph_t g(eng.kind());
    g.add_op(&relu_op);
    g.build_graph();

    auto pimpl = std::make_shared<impl::dnnl_impl::dnnl_partition_impl_t>(
            eng.kind(), impl::fpmath_mode::strict, impl::partition_kind::undef);
    pimpl->add_op(std::make_shared<impl::op_t>(relu_op));
    pimpl->init(&relu_op);

    impl::partition_t p;
    p.init(pimpl);

    impl::compiled_partition_t cp(p);
    ASSERT_EQ(p.id(), cp.src_partition().id());

    impl::logical_tensor_t lt_in_additional1 = utils::logical_tensor_init(
            /* tid= */ 3, {1, 1, 3, 3}, impl::data_type::f32);
    impl::logical_tensor_t lt_in_additional2 = utils::logical_tensor_init(
            /* tid= */ 4, {1, 1, 3, 3}, impl::data_type::f32);
    impl::logical_tensor_t lt_out_additional1
            = utils::logical_tensor_init(/* tid= */ 5, {1, 1, 3, 3},
                    impl::data_type::f32, impl::layout_type::any);
    impl::logical_tensor_t lt_out_additional2
            = utils::logical_tensor_init(/* tid= */ 6, {1, 1, 3, 3},
                    impl::data_type::f32, impl::layout_type::any);

    // in/outputs list have to contain required logical tensor
    std::vector<const impl::logical_tensor_t *> lt_inputs_wrong {
            &lt_in_additional1, &lt_in_additional2}; // no required
    std::vector<const impl::logical_tensor_t *> lt_outputs_wrong {
            &lt_out_additional1, &lt_out_additional2}; // no required

    // compile function return a miss_ins_outs error, since it can't find
    // required inputs and outputs from the given arguments
    impl::status_t status
            = p.compile(&cp, lt_inputs_wrong, lt_outputs_wrong, &eng);
    ASSERT_EQ(status, impl::status::invalid_arguments);

    // in/outputs list can contain more logical tensors than required
    std::vector<const impl::logical_tensor_t *> lt_inputs_correct {
            &lt_in_additional1, /* required */ &lt_in, &lt_in_additional2};
    std::vector<const impl::logical_tensor_t *> lt_outputs_correct {
            &lt_out_additional1, &lt_out_additional2, /* required */ &lt_out};

    // compile function will search its required inputs and outputs by itself
    status = p.compile(&cp, lt_inputs_correct, lt_outputs_correct, &eng);
    ASSERT_EQ(status, impl::status::success);

    //query logical_tensor to get its layout
    impl::logical_tensor_t query_lt_in, query_lt_out;
    ASSERT_EQ(cp.query_logical_tensor(lt_out.id, &query_lt_out),
            impl::status::success);
    ASSERT_EQ(query_lt_out.layout_type, impl::layout_type::strided);

    size_t size_in = 0, size_out = 0;
    cp.query_logical_tensor(lt_in.id, &query_lt_in);
    size_in = impl::logical_tensor_wrapper_t(query_lt_in).size();
    size_out = impl::logical_tensor_wrapper_t(query_lt_out).size();
    ASSERT_EQ(size_in, 9 * sizeof(float));
    ASSERT_EQ(size_in, size_out);

    size_t ele_num_in = size_in / sizeof(float);
    size_t ele_num_out = size_out / sizeof(float);
    test::vector<float> data_in(ele_num_in);
    test::vector<float> data_out(ele_num_out);
    for (size_t i = 0; i < ele_num_in; i++) {
        data_in[i] = static_cast<float>(i) - static_cast<float>(ele_num_in / 2);
    }

    impl::tensor_t t_in(lt_in, &eng, data_in.data()),
            t_out(query_lt_out, &eng, data_out.data());
    impl::tensor_t t_in_additional1(lt_in_additional1, &eng, nullptr),
            t_in_additional2(lt_in_additional2, &eng, nullptr);
    impl::tensor_t t_out_additional1(lt_out_additional1, &eng, nullptr),
            t_out_additional2(lt_out_additional2, &eng, nullptr);

    // when submit, in/outputs tensor's order must be same as compile
    // funcstion's in/outputs logical tensor
    std::vector<impl::tensor_t> t_inputs_correct {
            t_in_additional1, t_in, t_in_additional2};
    std::vector<impl::tensor_t> t_outputs_correct {
            t_out_additional1, t_out_additional2, t_out};

    impl::stream_t &strm = get_stream();
    EXPECT_SUCCESS(cp.execute(&strm, t_inputs_correct, t_outputs_correct));
    strm.wait();

    test::vector<float> ref_out(ele_num_in);
    for (size_t i = 0; i < ele_num_in; i++) {
        ref_out[i] = (i < ele_num_in / 2)
                ? 0.0f
                : static_cast<float>(i) - static_cast<float>(ele_num_in / 2);
    }

    for (size_t i = 0; i < ele_num_in; i++) {
        ASSERT_FLOAT_EQ(ref_out[i], data_out[i]);
    }
}

TEST(CompiledPartition, AllowRepeatedInputs) {
    impl::engine_t &eng = get_engine();

    impl::op_t n(impl::op_kind::Multiply);

    impl::logical_tensor_t lt_in1 = utils::logical_tensor_init(
            /* tid= */ 1, {1, 1, 3, 3}, impl::data_type::f32);
    impl::logical_tensor_t lt_out = utils::logical_tensor_init(/* tid= */ 2,
            {1, 1, 3, 3}, impl::data_type::f32, impl::layout_type::any);

    // repeated inputs
    n.add_input(lt_in1);
    n.add_input(lt_in1);
    n.add_output(lt_out);

    auto pimpl = std::make_shared<impl::dnnl_impl::dnnl_partition_impl_t>(
            eng.kind(), impl::fpmath_mode::strict, impl::partition_kind::undef);
    pimpl->add_op(std::make_shared<impl::op_t>(n));
    pimpl->init(&n);

    impl::partition_t p;
    p.init(pimpl);

    impl::compiled_partition_t cp(p);

    // only one input
    std::vector<const impl::logical_tensor_t *> lt_ins {&lt_in1};
    std::vector<const impl::logical_tensor_t *> lt_outs {&lt_out};

    impl::status_t status = p.compile(&cp, lt_ins, lt_outs, &eng);
    ASSERT_EQ(status, impl::status::success);

    impl::logical_tensor_t query_lt_out;
    ASSERT_EQ(cp.query_logical_tensor(lt_out.id, &query_lt_out),
            impl::status::success);
    ASSERT_EQ(query_lt_out.layout_type, impl::layout_type::strided);

    size_t size_in = 0, size_out = 0;
    size_in = impl::logical_tensor_wrapper_t(lt_in1).size();
    size_out = impl::logical_tensor_wrapper_t(query_lt_out).size();
    ASSERT_EQ(size_in, 9 * sizeof(float));
    ASSERT_EQ(size_in, size_out);

    test::vector<float> data_in {
            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
    test::vector<float> data_out(data_in.size());
    test::vector<float> ref_out {
            1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f, 49.0f, 64.0f, 81.0f};

    impl::tensor_t t_in1(lt_in1, &eng, data_in.data());
    impl::tensor_t t_out(query_lt_out, &eng, data_out.data());

    // only one input
    std::vector<impl::tensor_t> t_ins {t_in1};
    std::vector<impl::tensor_t> t_outs {t_out};

    impl::stream_t &strm = get_stream();
    EXPECT_SUCCESS(cp.execute(&strm, t_ins, t_outs));
    strm.wait();

    for (size_t i = 0; i < ref_out.size(); i++) {
        ASSERT_FLOAT_EQ(ref_out[i], data_out[i]);
    }
}
