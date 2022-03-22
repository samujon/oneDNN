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

#include <memory>
#include <string>
#include <vector>

#include "dnnl.hpp"

#include "interface/c_types_map.hpp"
#include "interface/value.hpp"

#include "backend/dnnl/common.hpp"
#include "backend/dnnl/passes/op_executable.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {
using op_t = impl::op_t;
using op_ptr = std::shared_ptr<impl::op_t>;
using value_ptr = std::shared_ptr<impl::value_t>;
using ltw = impl::logical_tensor_wrapper_t;

static value_ptr insert_workspace(op_ptr &op) {
    logical_tensor_t lt = impl::empty_logical_tensor_with_default_id();
    value_ptr workspace_val
            = std::make_shared<value_t>(*op, op->num_outputs(), lt);
    op->add_output(workspace_val);
    return workspace_val;
}

static inline void insert_reorder_before(op_ptr &op, size_t offset,
        const dnnl::memory::desc &opt_mdesc, const dnnl::engine &p_engine,
        primitive_attr_mgr_t &prm_attr_mgr, std::vector<op_ptr> &reorder_ops) {
    value_ptr in_val = op->get_input_value(offset);
    const logical_tensor_t &in_lt = in_val->get_logical_tensor();
    // just return if real input layout is the same as optimal layout or
    // input layout type is ANY
    if (make_dnnl_memory_desc(in_lt) == opt_mdesc || ltw(in_lt).is_any())
        return;

    // create reorder op, connect it to graph and add it's scratchpad output
    auto reorder_op = std::make_shared<op_t>(op_kind::dnnl_reorder);
    insert_op_before(reorder_op, op, offset);
    auto scratchpad_val = insert_empty_scratchpad(reorder_op);
    reorder_ops.emplace_back(reorder_op);
    // set optimal layout to reorder's output
    auto reorder_out_val = reorder_op->get_output_value(0);
    fill_layout_info(reorder_out_val, opt_mdesc);
    // fill shape info
    reorder_out_val->set_data_type(ltw(in_lt).data_type());
    reorder_out_val->set_dims(ltw(in_lt).vdims());

    // set layout info for scratchpad output
    const auto &pd = create_reorder_pd(reorder_op, p_engine, prm_attr_mgr);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static inline void insert_reorder_after(op_ptr &op, size_t offset,
        const dnnl::memory::desc &opt_mdesc, const dnnl::engine &p_engine,
        primitive_attr_mgr_t &prm_attr_mgr, std::vector<op_ptr> &reorder_ops) {
    value_ptr out_val = op->get_output_value(offset);
    const logical_tensor_t &out_lt = out_val->get_logical_tensor();
    // just return if real output layout is the same as optimal layout or
    // output layout type is ANY
    if (make_dnnl_memory_desc(out_lt) == opt_mdesc || ltw(out_lt).is_any())
        return;

    // create reorder op, connect it to graph and add it's scratchpad output
    auto reorder_op = std::make_shared<op_t>(op_kind::dnnl_reorder);
    insert_op_after(reorder_op, op, offset);
    auto scratchpad_val = insert_empty_scratchpad(reorder_op);
    reorder_ops.emplace_back(reorder_op);
    // set optimal layout to reorder's input
    auto reorder_in_val = reorder_op->get_input_value(0);
    fill_layout_info(reorder_in_val, opt_mdesc);
    // fill shape info
    reorder_in_val->set_data_type(ltw(out_lt).data_type());
    reorder_in_val->set_dims(ltw(out_lt).vdims());

    // set layout info for scratchpad output
    const auto &pd = create_reorder_pd(reorder_op, p_engine, prm_attr_mgr);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_conv(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const bool is_dw = op->get_kind() == op_kind::dnnl_conv_depthwise;
    // always create pd using any format
    const auto &pd_flag_pair
            = create_conv_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    // insert reorders for conv's inputs
    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    if (op->has_attr("with_bias") && op->get_attr<bool>("with_bias")) {
        insert_reorder_before(
                op, 2, pd.bias_desc(), p_engine, prm_attr_mgr, reorder_ops);
        value_ptr bias = op->get_input_value(2);
        fill_layout_info(bias, pd.bias_desc());
    } else if (is_dw) {
        const auto &dw_wei_opt_mdesc = pd.query_md(query::exec_arg_md,
                DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_WEIGHTS);
        insert_reorder_before(
                op, 2, dw_wei_opt_mdesc, p_engine, prm_attr_mgr, reorder_ops);
        value_ptr dw_wei = op->get_input_value(2);
        fill_layout_info(dw_wei, dw_wei_opt_mdesc);
    }
    // insert a reorder if output layout is different from output optimal layout
    // 1) output layout is opaque
    // 2) output is any, directly set optimal layout
    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    // according to op schema, scratchpad must be be second output
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as conv's last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_deconv(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_deconv_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    // insert reorders for deconv's inputs
    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    if (op->has_attr("with_bias") && op->get_attr<bool>("with_bias")) {
        insert_reorder_before(
                op, 2, pd.bias_desc(), p_engine, prm_attr_mgr, reorder_ops);
        value_ptr bias = op->get_input_value(2);
        fill_layout_info(bias, pd.bias_desc());
    }
    // insert a reorder if output layout is different from output optimal layout
    // 1) output layout is opaque
    // 2) output is any, directly set optimal layout
    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as conv's last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_deconv_bwd_data(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    // always create pd using any format
    const auto &pd_flag_pair
            = create_deconv_bwd_data_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    // insert reorders for inputs
    insert_reorder_before(
            op, 0, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(0);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    // insert a reorder if output layout is different from output optimal layout
    // 1) output layout is opaque
    // 2) output is any, directly set optimal layout
    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    // according to op schema, scratchpad must be be second output
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as deconv's bwd last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_deconv_bwd_weights(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair = create_deconv_bwd_weights_pd(
            op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(1);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_after(
            op, 0, pd.diff_weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_weights = op->get_output_value(0);
    fill_layout_info(diff_weights, pd.diff_weights_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as deconv's bwd last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_eltwise(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    // When input's layout is specified (opaque or strided),
    // we can propagate it to output.
    const auto &pd_flag_pair
            = create_eltwise_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_eltwise_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_eltwise_bwd_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    // to hit an optimized kernel, input/output of forward and both diff_dst
    // and diff_src should use the same memory format. Primitive is created
    // based on a backward data and here we are contidionally aligning forward
    // data format.
    auto opt_desc = (op->has_attr("use_dst") && op->get_attr<bool>("use_dst"))
            ? pd.dst_desc()
            : pd.src_desc();
    insert_reorder_before(op, 0, opt_desc, p_engine, prm_attr_mgr, reorder_ops);
    value_ptr data = op->get_input_value(0);
    fill_layout_info(data, opt_desc);

    insert_reorder_before(
            op, 1, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(1);
    fill_layout_info(diff_dst, opt_desc);

    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_binary(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_binary_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_concat(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        std::vector<op_ptr> &reorder_ops) {
    auto pd = create_concat_pd(op, p_engine, prm_attr_mgr);

    for (size_t i = 0; i < op->num_inputs(); ++i) {
        insert_reorder_before(op, i, pd.src_desc(static_cast<int>(i)), p_engine,
                prm_attr_mgr, reorder_ops);
        fill_layout_info(
                op->get_input_value(i), pd.src_desc(static_cast<int>(i)));
    }

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    fill_layout_info(op->get_output_value(0), pd.dst_desc());

    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_shuffle(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_shuffle_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    value_ptr src = op->get_input_value(0);
    value_ptr dst = op->get_output_value(0);

    assertm(!ltw(src->get_logical_tensor()).is_any(),
            "shuffle's src can't be any layout");

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    fill_layout_info(dst, pd.dst_desc());

    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_matmul(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_matmul_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    // insert reorders for matmul's inputs
    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    if (op->has_attr("with_bias") && op->get_attr<bool>("with_bias")) {
        insert_reorder_before(
                op, 2, pd.bias_desc(), p_engine, prm_attr_mgr, reorder_ops);
        value_ptr bias = op->get_input_value(2);
        fill_layout_info(bias, pd.bias_desc());
    }
    // insert a reorder if output layout is different from output optimal layout
    // 1) output layout is opaque
    // 2) output is any, directly set optimal layout
    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as matmul's last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_pool(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_pool_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // make scratchpad as pool's last output
    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());

    if (op->has_attr("is_training") && op->get_attr<bool>("is_training")) {
        value_ptr workspace_val = op->get_output_value(2);
        const memory::desc &ws_md = pd.workspace_desc();
#ifdef DNNL_GRAPH_LAYOUT_DEBUG
        workspace_val->set_dims(ws_md.dims());
        workspace_val->set_data_type(
                static_cast<impl::data_type_t>(ws_md.data.data_type));
#endif
        fill_layout_info(workspace_val, ws_md);
    }
}

static void layout_propagation_for_pool_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_pool_bwd_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(0);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    // make scratchpad as pool's last output
    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_batchnorm(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_batchnorm_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    if (op->get_attr<bool>("is_training")) {
        value_ptr running_mean = op->get_output_value(1);
        value_ptr running_variance = op->get_output_value(2);
        value_ptr batch_mean = op->get_output_value(3);
        value_ptr batch_variance = op->get_output_value(4);

        fill_layout_info(running_mean, pd.mean_desc());
        fill_layout_info(running_variance, pd.variance_desc());
        fill_layout_info(batch_mean, pd.mean_desc());
        fill_layout_info(batch_variance, pd.variance_desc());
    }

    // make scratchpad as batchnorm's last output
    value_ptr scratchpad_val = op->get_output_values().back();
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
    // if batchnorm's prop_kind is forward_training and fused with ReLU
    if (op->has_attr("fuse_relu") && op->get_attr<bool>("fuse_relu")) {
        value_ptr workspace_val = insert_workspace(op);
        fill_layout_info(workspace_val, pd.workspace_desc());
    }
}

static void layout_propagation_for_batchnorm_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    if (op->num_inputs() != 5 || op->num_outputs() != 3) {
        assert(!"Currently, only support use_scale and use_shift mode!");
    }
    const auto &pd_flag_pair
            = create_batchnorm_bwd_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(1);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_before(
            op, 3, pd.mean_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr mean = op->get_input_value(3);
    fill_layout_info(mean, pd.mean_desc());

    insert_reorder_before(
            op, 4, pd.variance_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr var = op->get_input_value(4);
    fill_layout_info(var, pd.variance_desc());

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.diff_src_desc());

    value_ptr diff_gamma = op->get_output_value(1);
    value_ptr diff_beta = op->get_output_value(2);

    fill_layout_info(diff_gamma, pd.diff_weights_desc());
    fill_layout_info(diff_beta, pd.diff_weights_desc());

    // make scratchpad as batchnorm's last output
    value_ptr scratchpad_val = insert_empty_scratchpad(op);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_prelu(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_prelu_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    value_ptr scratchpad_val = op->get_output_value(1);
    // make scratchpad as prelu's last output
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_prelu_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_prelu_bwd_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    value_ptr diff_dst = op->get_input_value(2);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_input_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    insert_reorder_after(
            op, 1, pd.diff_weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_wei = op->get_input_value(1);
    fill_layout_info(diff_wei, pd.diff_weights_desc());

    value_ptr scratchpad_val = op->get_output_value(2);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_layernorm(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_layernorm_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    if (op->num_outputs() > 2) {
        // keep_stats is true
        value_ptr mean = op->get_output_value(1);
        value_ptr variance = op->get_output_value(2);
        fill_layout_info(mean, pd.mean_desc());
        fill_layout_info(variance, pd.variance_desc());
    }

    // scratchpad is layernorm's last output
    value_ptr scratchpad_val = op->get_output_values().back();
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_layernorm_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    assertm(op->num_inputs() == 4 || op->num_inputs() == 6,
            "Currently, we can have either both use_scale and use_shift modes "
            "enabled or disabled!");

    const auto &pd_flag_pair
            = create_layernorm_bwd_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    size_t in_index {0};
    insert_reorder_before(
            op, in_index, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(in_index++);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(op, in_index, pd.diff_dst_desc(), p_engine,
            prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(in_index++);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_before(
            op, in_index, pd.mean_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr mean = op->get_input_value(in_index++);
    fill_layout_info(mean, pd.mean_desc());

    insert_reorder_before(op, in_index, pd.variance_desc(), p_engine,
            prm_attr_mgr, reorder_ops);
    value_ptr var = op->get_input_value(in_index++);
    fill_layout_info(var, pd.variance_desc());

    size_t out_index {0};
    insert_reorder_after(op, out_index, pd.diff_src_desc(), p_engine,
            prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(out_index++);
    fill_layout_info(diff_src, pd.diff_src_desc());

    if (op->get_attr<bool>("with_gamma")) {
        const auto &scale_opt_mdesc
                = pd.query_md(query::exec_arg_md, DNNL_ARG_SCALE);
        insert_reorder_before(op, in_index, scale_opt_mdesc, p_engine,
                prm_attr_mgr, reorder_ops);
        value_ptr scale = op->get_input_value(in_index++);
        fill_layout_info(scale, scale_opt_mdesc);

        const auto &diff_scale_opt_mdesc
                = pd.query_md(query::exec_arg_md, DNNL_ARG_DIFF_SCALE);
        insert_reorder_after(op, out_index, diff_scale_opt_mdesc, p_engine,
                prm_attr_mgr, reorder_ops);
        value_ptr diff_scale = op->get_output_value(out_index++);
        fill_layout_info(diff_scale, diff_scale_opt_mdesc);
    }
    if (op->get_attr<bool>("with_beta")) {
        const auto &shift_opt_mdesc
                = pd.query_md(query::exec_arg_md, DNNL_ARG_SHIFT);
        insert_reorder_before(op, in_index, shift_opt_mdesc, p_engine,
                prm_attr_mgr, reorder_ops);
        value_ptr shift = op->get_input_value(in_index++);
        fill_layout_info(shift, shift_opt_mdesc);

        const auto &diff_shift_opt_mdesc
                = pd.query_md(query::exec_arg_md, DNNL_ARG_DIFF_SHIFT);
        insert_reorder_after(op, out_index, diff_shift_opt_mdesc, p_engine,
                prm_attr_mgr, reorder_ops);
        value_ptr diff_shift = op->get_output_value(out_index++);
        fill_layout_info(diff_shift, diff_shift_opt_mdesc);
    }

    auto scratchpad_val = op->get_output_value(op->num_outputs() - 1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_permute(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        std::vector<op_ptr> &reorder_ops) {
    std::shared_ptr<impl::value_t> src, dst;
    src = op->get_input_value(0);
    dst = op->get_output_value(0);

    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
        dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
        dnnl::memory::desc out_md;

        auto permute_kind = op->get_attr<std::string>("permute_kind");
        if (permute_kind == "transpose") {
            // transpose the right-most two dims
            out_md = permute_last_two_dims(in_md);
        } else {
            auto from_format = op->get_attr<std::string>("from_format");
            auto to_format = op->get_attr<std::string>("to_format");
            if (from_format == "NCX" && to_format == "NXC") {
                out_md = permute_NCX2NXC(in_md);
            } else if (from_format == "NXC" && to_format == "NCX") {
                out_md = permute_NXC2NCX(in_md);
            } else if (from_format == "XIO" && to_format == "OIX") {
                out_md = permute_XIO2OIX(in_md);
            } else if (from_format == "OIX" && to_format == "XIO") {
                out_md = permute_OIX2XIO(in_md);
            } else {
                assertm(false, "not a supported permutation");
            }
        }

        fill_layout_info(dst, out_md);
    } else if (!ltw(out_lt).is_any() && ltw(in_lt).is_any()) {
        dnnl::memory::desc out_md = make_dnnl_memory_desc(out_lt);
        dnnl::memory::desc in_md;

        auto permute_kind = op->get_attr<std::string>("permute_kind");
        if (permute_kind == "transpose") {
            // transpose the right-most two dims
            in_md = permute_last_two_dims(out_md);
        } else {
            auto from_format = op->get_attr<std::string>("from_format");
            auto to_format = op->get_attr<std::string>("to_format");
            if (from_format == "NCX" && to_format == "NXC") {
                in_md = permute_NXC2NCX(out_md);
            } else if (from_format == "NXC" && to_format == "NCX") {
                in_md = permute_NCX2NXC(out_md);
            } else if (from_format == "XIO" && to_format == "OIX") {
                // for the case like conv's weight is set to ANY layout, need
                // propagate output layout to input
                in_md = permute_OIX2XIO(out_md);
            } else if (from_format == "OIX" && to_format == "XIO") {
                in_md = permute_XIO2OIX(out_md);
            } else {
                assertm(false, "not a supported permutation");
            }
        }

        fill_layout_info(src, in_md);
    } else if (!ltw(in_lt).is_any() && !ltw(out_lt).is_any()) {
        // case `conv (opaque) -> permute -> output (strided)` or
        // case `input (strided) -> permute -> conv (opaque)`
        dnnl::memory::desc out_md = make_dnnl_memory_desc(out_lt);
        dnnl::memory::desc tmp_in_md;

        auto permute_kind = op->get_attr<std::string>("permute_kind");
        if (permute_kind == "transpose") {
            // transpose the right-most two dims
            tmp_in_md = permute_last_two_dims(out_md);
        } else {
            auto from_format = op->get_attr<std::string>("from_format");
            auto to_format = op->get_attr<std::string>("to_format");
            if (from_format == "NCX" && to_format == "NXC") {
                tmp_in_md = permute_NXC2NCX(out_md);
            } else if (from_format == "NXC" && to_format == "NCX") {
                tmp_in_md = permute_NCX2NXC(out_md);
            } else if (from_format == "XIO" && to_format == "OIX") {
                // This is required when layout propagation is done more than
                // once. At the first time, permute's input layout is strided
                // while output layout is set to opaque. At the second time,
                // we need infer the input layout to avoid inserting the reorder
                tmp_in_md = permute_OIX2XIO(out_md);
            } else if (from_format == "OIX" && to_format == "XIO") {
                // This is required when layout propagation is done more than
                // once. At the first time, permute's input layout is strided
                // while output layout is set to opaque. At the second time,
                // we need infer the input layout to avoid inserting the reorder
                tmp_in_md = permute_XIO2OIX(out_md);
            } else {
                assertm(false, "not a supported permutation");
            }
        }

        // if the input md derived from output md is different from the real
        // input mem desc, just insert a reorder before the op
        if (make_dnnl_memory_desc(in_lt) != tmp_in_md)
            insert_reorder_before(
                    op, 0, tmp_in_md, p_engine, prm_attr_mgr, reorder_ops);
    }
}

static void layout_propagation_for_to_group(op_ptr &op) {
    std::shared_ptr<impl::value_t> src, dst;
    src = op->get_input_value(0);
    dst = op->get_output_value(0);
    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
        dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
        dnnl::memory::desc out_md;
        auto groups = op->get_attr<int64_t>("groups");
        if (op->has_attr("is_convtranspose")
                && op->get_attr<bool>("is_convtranspose")) {
            auto permuted_weight = transpose(in_md, 0, 1);
            auto permuted_group_weight = to_grouped(permuted_weight, groups);
            out_md = transpose(permuted_group_weight, 1, 2);
        } else {
            out_md = to_grouped(in_md, groups);
        }
        fill_layout_info(dst, out_md);
    }
}

static void layout_propagation_for_from_group(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        std::vector<op_ptr> &reorder_ops) {
    const auto get_dst_md
            = [](const dnnl::memory::desc &src_md,
                      bool is_convtranspose) -> dnnl::memory::desc {
        if (is_convtranspose) {
            auto permuted_dst = transpose(src_md, 1, 2);
            auto permuted_dst_no_groups = from_grouped(permuted_dst);
            return transpose(permuted_dst_no_groups, 0, 1);
        } else {
            return from_grouped(src_md);
        }
    };

    value_ptr src = op->get_input_value(0);
    value_ptr dst = op->get_output_value(0);
    auto src_lt = src->get_logical_tensor();
    auto dst_lt = dst->get_logical_tensor();

    if (ltw(src_lt).is_any()) return;

    const bool is_convtranspose = op->has_attr("is_convtranspose")
            ? op->get_attr<bool>("is_convtranspose")
            : false;
    const auto src_md = make_dnnl_memory_desc(src_lt);
    dnnl::memory::desc infered_dst_md = get_dst_md(src_md, is_convtranspose);
    // from_grouped uses the 'allow_empty' option when reshaping, so if
    // reshape will not succeed (e.g. padding exists inside dims we want
    // to join), infered_dst_md will be an empty memory descriptor.
    if (infered_dst_md.is_zero()) {
        dnnl::memory::desc strided_dst_md(src_md.dims(), src_md.data_type(),
                get_dense_strides(src_md.dims()));
        insert_reorder_before(
                op, 0, strided_dst_md, p_engine, prm_attr_mgr, reorder_ops);
        infered_dst_md = get_dst_md(strided_dst_md, is_convtranspose);
    }

    if (ltw(dst_lt).is_any()) {
        fill_layout_info(dst, infered_dst_md);
    } else {
        insert_reorder_after(
                op, 0, infered_dst_md, p_engine, prm_attr_mgr, reorder_ops);
    }
}

static void layout_propagation_for_reshape(op_ptr &op) {
    std::shared_ptr<impl::value_t> src, dst;
    src = op->get_input_value(0);
    dst = op->get_output_value(0);
    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
        dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
        dnnl::memory::desc out_md = in_md;
        auto target_dims = make_dnnl_memory_desc(out_lt).dims();
        out_md = in_md.reshape(target_dims);
        fill_layout_info(dst, out_md);
    }
}

static void layout_propagation_for_transpose(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        std::vector<op_ptr> &reorder_ops) {
    std::shared_ptr<impl::value_t> src, dst;
    src = op->get_input_value(0);
    dst = op->get_output_value(0);
    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    assertm(!ltw(in_lt).is_any(), "transpose's src can't be any layout now");

    // calculate the expected transposed layout by permuting the md
    dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
    auto order = op->get_attr<std::vector<int64_t>>("order");
    std::vector<int> axes;
    axes.reserve(order.size());
    for (int64_t &x : order) {
        axes.emplace_back(static_cast<int>(x));
    }

    dnnl::memory::desc expected_out_md = in_md.permute_axes(axes);
    if (ltw(out_lt).is_any()) {
        fill_layout_info(dst, expected_out_md);
    } else {
        // if the output layout is specified, we need to check if it's matched
        // with the expected out layout. If not, we should insert a reorder op
        // to convert the transposed layout to the specified one.
        dnnl::memory::desc out_md = make_dnnl_memory_desc(out_lt);
        if (expected_out_md != out_md) {
            insert_reorder_after(op, 0, expected_out_md, p_engine, prm_attr_mgr,
                    reorder_ops);
        }
    }
}

static void layout_propagation_for_expand(op_ptr &op) {
    value_ptr src = op->get_input_value(0);
    value_ptr dst = op->get_output_value(0);
    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
        dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
        // 'out_lt' shape should be known at this stage
        fill_layout_info(dst, in_md.reshape(ltw(out_lt).vdims()));
    }
}

static void layout_propagation_for_squeeze(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        std::vector<op_ptr> &reorder_ops) {
    std::shared_ptr<impl::value_t> src, dst;
    src = op->get_input_value(0);
    dst = op->get_output_value(0);
    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    auto predecessor_kind = src->get_producer().get_kind();
    if (predecessor_kind == op_kind::dnnl_reduction) {
        auto in_md = make_dnnl_memory_desc(in_lt);
        if (ltw(out_lt).is_any()) {
            const auto in_ndim = static_cast<int64_t>(in_md.dims().size());
            auto axes = op->get_attr<std::vector<int64_t>>("axes");
            std::transform(axes.begin(), axes.end(), axes.begin(),
                    [&in_ndim](int64_t axis) -> int64_t {
                        return axis < 0 ? in_ndim + axis : axis;
                    });
            for (const auto axis : axes) {
                const bool valid
                        = in_md.data.padded_dims[axis] == in_md.data.dims[axis]
                        && in_md.data.dims[axis] == 1;
                UNUSED(valid);
                assertm(valid,
                        "squeeze failed since there is padding in the squeezed "
                        "dimension");
            }
            fill_layout_info(dst, in_md.reshape(ltw(out_lt).vdims()));
        } else {
            auto out_md = make_dnnl_memory_desc(out_lt);
            auto tmp_in_md = out_md.reshape(in_md.dims());
            if (in_md != tmp_in_md)
                insert_reorder_before(
                        op, 0, tmp_in_md, p_engine, prm_attr_mgr, reorder_ops);
        }
        return;
    }

    if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
        // `matmul (opaque) -> squeeze -> output (any)`
        dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
        dnnl::memory::desc out_md = in_md;

        auto axes = op->get_attr<std::vector<int64_t>>("axes");
        auto in_ndim = in_md.dims().size();

        // If there is padding in the dimension to be squeezed, reshape
        // cannot handle this case and we need insert a reorder for this
        // case
        const auto &blocking_desc = in_md.data.format_desc.blocking;
        for (size_t i = 0; i < blocking_desc.inner_nblks; ++i) {
            if (blocking_desc.inner_idxs[i] == in_ndim + axes.back()) {
                assertm(false,
                        "squeeze failed since there is padding in the squeezed "
                        "dimension");
            }
        }

        if (axes.size() == 2) {
            // the output is a scalar or no need to squeeze,
            // just skip for layout propagation
        } else {
            assertm(axes.size() == 1,
                    "the size of axes in squeeze op "
                    "should be 1.");
            auto reshaped_dims = in_md.dims();
            if (axes.back() == -1) {
                reshaped_dims.erase(reshaped_dims.begin() + in_ndim - 1);
            } else if (axes.back() == -2) {
                reshaped_dims.erase(reshaped_dims.begin() + in_ndim - 2);
            }
            out_md = in_md.reshape(reshaped_dims);
        }

        fill_layout_info(dst, out_md);
    } else if (!ltw(in_lt).is_any() && !ltw(out_lt).is_any()) {
        // `matmul (opaque) -> squeeze -> output (strided)`
        dnnl::memory::desc out_md = make_dnnl_memory_desc(out_lt);
        dnnl::memory::desc tmp_in_md = out_md;

        auto axes = op->get_attr<std::vector<int64_t>>("axes");

        if (axes.empty() || axes.size() == 2) {
            // the output is a scalar or no need to squeeze,
            // just skip for layout propagation
        } else {
            assertm(axes.size() == 1,
                    "the size of axes in squeeze op "
                    "should be 1.");
            auto reshaped_dims = out_md.dims();
            if (axes.back() == -1) {
                reshaped_dims.insert(reshaped_dims.end(), 1);
            } else if (axes.back() == -2) {
                reshaped_dims.insert(reshaped_dims.end() - 1, 1);
            }
            tmp_in_md = out_md.reshape(reshaped_dims);
        }

        // if the input md derived from output md is different from the real
        // input mem desc, just insert a reorder before the op
        if (make_dnnl_memory_desc(in_lt) != tmp_in_md)
            insert_reorder_before(
                    op, 0, tmp_in_md, p_engine, prm_attr_mgr, reorder_ops);
    }
}

static void layout_propagation_for_reorder(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr) {
    std::shared_ptr<impl::value_t> src, dst;
    src = op->get_input_value(0);
    dst = op->get_output_value(0);
    auto in_lt = src->get_logical_tensor();
    auto out_lt = dst->get_logical_tensor();

    if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
        assertm(!op->has_attr("change_layout")
                        || !op->get_attr<bool>("change_layout"),
                "the dnnl_reorder op's input and output layout must be known "
                "if it changes layout");

        auto out_md = make_dnnl_memory_desc(in_lt);
        out_md.data.data_type
                = static_cast<dnnl_data_type_t>(ltw(out_lt).data_type());
        fill_layout_info(dst, out_md);
    } else if (!ltw(out_lt).is_any() && ltw(in_lt).is_any()) {
        assertm(!op->has_attr("change_layout")
                        || !op->get_attr<bool>("change_layout"),
                "the dnnl_reorder op's input and output layout must be known "
                "if it changes layout");

        auto in_md = make_dnnl_memory_desc(out_lt);
        in_md.data.data_type
                = static_cast<dnnl_data_type_t>(ltw(in_lt).data_type());
        fill_layout_info(src, in_md);
    }

    // set layout info for scratchpad output
    if (op->num_outputs() == 1) { insert_empty_scratchpad(op); }
    const auto &pd = create_reorder_pd(op, p_engine, prm_attr_mgr);
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_mul_scales(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr) {
    layout_propagation_for_reorder(op, p_engine, prm_attr_mgr);
}

static void layout_propagation_for_bn_folding(
        op_ptr &op, const dnnl::engine &p_engine) {
    // skip the scratchpad
    for (size_t i = 0; i < op->num_outputs() - 1; i++) {
        auto in_lt = op->get_input_value(i)->get_logical_tensor();
        auto out_lt = op->get_output_value(i)->get_logical_tensor();
        if (!ltw(in_lt).is_any() && ltw(out_lt).is_any()) {
            dnnl::memory::desc in_md = make_dnnl_memory_desc(in_lt);
            auto dst = op->get_output_value(i);
            fill_layout_info(dst, in_md);
        }
    }

    auto prm = std::make_shared<bn_folding_t>(op, p_engine);
    // scratchpad is bn_folding's last inputs
    auto val = op->get_output_value(2);
    fill_layout_info(val, prm->scratchpad_desc());
}

static void layout_propagation_for_conv_bwd_data(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_conv_bwd_data_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(0);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_before(
            op, 1, pd.weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr wei = op->get_input_value(1);
    fill_layout_info(wei, pd.weights_desc());

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as conv's last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_conv_bwd_weights(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_conv_bwd_weights_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr src = op->get_input_value(0);
    fill_layout_info(src, pd.src_desc());

    insert_reorder_before(
            op, 1, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(1);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_after(
            op, 0, pd.diff_weights_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_weights = op->get_output_value(0);
    fill_layout_info(diff_weights, pd.diff_weights_desc());

    // fill scratchpads dimensions and data type to scratchpad value_t
    auto scratchpad_val = op->get_output_value(1);
    const memory::desc scratchpad_desc = pd.scratchpad_desc();
    const memory::dims dims = scratchpad_desc.dims();
    scratchpad_val->set_dims(dims);
    scratchpad_val->set_data_type(
            static_cast<impl::data_type_t>(scratchpad_desc.data_type()));

    // make scratchpad as conv's last output
    fill_layout_info(scratchpad_val, scratchpad_desc);
}

static void layout_propagation_for_resampling(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_resampling_pd(op, p_engine, prm_attr_mgr, pd_cache);

    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // make scratchpad as resampling's last output
    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_resampling_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    const auto &pd_flag_pair
            = create_resampling_bwd_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    auto scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_dnnl_sum(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        std::vector<op_ptr> &reorder_ops) {
    value_ptr dst = op->get_output_value(0);
    bool input_has_any_format = false;
    for (const auto &in_val : op->get_input_values()) {
        if (ltw(in_val->get_logical_tensor()).is_any()) {
            input_has_any_format = true;
            break;
        }
    }

    MAYBE_UNUSED(input_has_any_format);
    assertm(!input_has_any_format,
            "input format of sum primitive cannot be any.");

    auto pd = create_dnnl_sum_pd(op, p_engine, prm_attr_mgr);

    if (ltw(dst->get_logical_tensor()).is_any()) {
        insert_reorder_after(
                op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
        dst = op->get_output_value(0);
        fill_layout_info(dst, pd.dst_desc());
    }

    // scratchpad is dnnl_sum's last output
    value_ptr scratchpad_val = op->get_output_values().back();
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_softmax(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    value_ptr src = op->get_input_value(0);
    assertm(!ltw(src->get_logical_tensor()).is_any(),
            "softmax's src can't be any layout now");

    const auto &pd_flag_pair = create_softmax_pd(op, p_engine, prm_attr_mgr,
            pd_cache, dnnl::algorithm::softmax_accurate);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // scratchpad is dnnl_softmax's last output
    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_softmax_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    value_ptr dst = op->get_input_value(1);
    assertm(!ltw(dst->get_logical_tensor()).is_any(),
            "softmax bwd's dst can't be any layout now");

    const auto &pd_flag_pair = create_softmax_bwd_pd(op, p_engine, prm_attr_mgr,
            pd_cache, dnnl::algorithm::softmax_accurate);

    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(0);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    // according to op schema, scratchpad must be be second output
    auto scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_logsoftmax(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    value_ptr src = op->get_input_value(0);
    assertm(!ltw(src->get_logical_tensor()).is_any(),
            "logsoftmax's src can't be any layout now");

    const auto &pd_flag_pair = create_softmax_pd(
            op, p_engine, prm_attr_mgr, pd_cache, dnnl::algorithm::softmax_log);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    // scratchpad is dnnl_logsoftmax's last output
    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_logsoftmax_bwd(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    value_ptr dst = op->get_input_value(1);
    assertm(!ltw(dst->get_logical_tensor()).is_any(),
            "logsoftmax bwd's dst can't be any layout now");

    const auto &pd_flag_pair = create_softmax_bwd_pd(
            op, p_engine, prm_attr_mgr, pd_cache, dnnl::algorithm::softmax_log);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_before(
            op, 0, pd.diff_dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_dst = op->get_input_value(0);
    fill_layout_info(diff_dst, pd.diff_dst_desc());

    insert_reorder_after(
            op, 0, pd.diff_src_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr diff_src = op->get_output_value(0);
    fill_layout_info(diff_src, pd.diff_src_desc());

    // according to op schema, scratchpad must be be second output
    auto scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void layout_propagation_for_reduction(op_ptr &op,
        const dnnl::engine &p_engine, primitive_attr_mgr_t &prm_attr_mgr,
        pd_cache_t &pd_cache, std::vector<op_ptr> &reorder_ops) {
    value_ptr src = op->get_input_value(0);
    assertm(!ltw(src->get_logical_tensor()).is_any(),
            "reduction's src can't be any layout now");

    const auto &pd_flag_pair
            = create_reduction_pd(op, p_engine, prm_attr_mgr, pd_cache);
    const auto &pd = pd_flag_pair.first;
    const auto is_first_time = pd_flag_pair.second;

    if (!is_first_time) return;

    insert_reorder_after(
            op, 0, pd.dst_desc(), p_engine, prm_attr_mgr, reorder_ops);
    value_ptr dst = op->get_output_value(0);
    fill_layout_info(dst, pd.dst_desc());

    value_ptr scratchpad_val = op->get_output_value(1);
    fill_layout_info(scratchpad_val, pd.scratchpad_desc());
}

static void remove_optional_conv_dw_output(
        std::vector<op_ptr> &subgraph, pd_cache_t &pd_cache) {
    std::vector<op_ptr> to_be_inserted_ops;
    std::vector<op_ptr> to_be_removed_ops;

    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != op_kind::dnnl_conv_depthwise) continue;

        op_ptr new_conv_dw
                = std::make_shared<impl::op_t>(op_kind::dnnl_conv_depthwise);
        new_conv_dw->merge_attributes(cur_op->get_attributes());

        for (size_t i = 0; i < cur_op->num_inputs(); ++i) {
            new_conv_dw->connect_input(i, cur_op->get_input_value(i));
        }
        // connect outputs, omit optional one with offset > 1
        value_ptr conv_dw_dst = cur_op->get_output_value(0);
        new_conv_dw->connect_output(0, conv_dw_dst);
        value_ptr scratchpad = cur_op->get_output_value(1);
        new_conv_dw->connect_output(1, scratchpad);

        auto pos = pd_cache.find(cur_op.get());
        if (pos != pd_cache.end()) {
            // we are replacing op, but we want to keep it's cached pd,
            // so later, during compile_ops execution, removed optional
            // output will not be required.
            auto &pd = pd_cache.at(cur_op.get());
            pd_cache.insert({new_conv_dw.get(), pd});
            pd_cache.erase(pos);
        }

        to_be_inserted_ops.emplace_back(new_conv_dw);
        to_be_removed_ops.emplace_back(cur_op);
    }

    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    for (const auto &op : to_be_removed_ops) {
        auto pos = std::find_if(subgraph.begin(), subgraph.end(),
                [op](const op_ptr &tmp) { return op.get() == tmp.get(); });
        if (pos != subgraph.end()) subgraph.erase(pos);
    }
}

/// This function is used to chooses optimal layout for computation bound op and
/// propagate the chosen optimal layout and given in/outputs layout in the
/// subgraph.
///
/// The workflow of layout propagation is:
///
/// Step1: propagate layout info according to the topological order
/// Step2: when comes to compute bound ops like Convolution/MatMul, it will
///     always use *any* format to create pd. And corresponding layout
///     propagation function will decide if insert a reorder based on comparsion
///     result between input/output layout and queried optimal layout
/// Step3: the following internal ops (permute/squeeze) will also be responsible
///     for deciding if insert a reorder before the op.
/// Step4: at the most cases the layout propagation should be done only once
///
/// \note The layout propagation function for each op should be bidirectional to
/// support propagating layout both from inputs to outputs and from outputs to
/// inputs.
impl::status_t layout_propagation(std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    const auto &p_engine = *(sg->p_engine_);
    auto &prm_attr_mgr = sg->prm_attr_mgr_;
    auto &pd_cache = sg->pd_cache_;

    // lambda function to do layout propagation for all ops
    auto layout_propagation_func = [&](std::shared_ptr<subgraph_t> &sg,
                                           std::vector<op_ptr> &reorder_ops) {
        impl::topo_order_visit(sg->get_output_ops(), [&](impl::op_t *op) {
            auto cur_op = op->shared_from_this();

            if (cur_op->get_kind() == op_kind::dnnl_convolution
                    || cur_op->get_kind() == op_kind::dnnl_conv_depthwise) {
                layout_propagation_for_conv(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_convtranspose) {
                layout_propagation_for_deconv(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind()
                    == op_kind::dnnl_convtranspose_bwd_data) {
                layout_propagation_for_deconv_bwd_data(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind()
                    == op_kind::dnnl_convtranspose_bwd_weights) {
                layout_propagation_for_deconv_bwd_weights(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_conv_bwd_data) {
                layout_propagation_for_conv_bwd_data(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_conv_bwd_weights) {
                layout_propagation_for_conv_bwd_weights(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_matmul) {
                layout_propagation_for_matmul(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_pool) {
                layout_propagation_for_pool(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_pool_bwd) {
                layout_propagation_for_pool_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_batchnorm) {
                layout_propagation_for_batchnorm(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_batchnorm_bwd) {
                layout_propagation_for_batchnorm_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_layernorm) {
                layout_propagation_for_layernorm(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_layernorm_bwd) {
                layout_propagation_for_layernorm_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_eltwise) {
                layout_propagation_for_eltwise(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_eltwise_bwd) {
                layout_propagation_for_eltwise_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_concat) {
                layout_propagation_for_concat(
                        cur_op, p_engine, prm_attr_mgr, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_prelu) {
                layout_propagation_for_prelu(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_prelu_bwd) {
                layout_propagation_for_prelu_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::permute) {
                layout_propagation_for_permute(
                        cur_op, p_engine, prm_attr_mgr, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_mul_scales) {
                layout_propagation_for_mul_scales(
                        cur_op, p_engine, prm_attr_mgr);
            } else if (cur_op->get_kind() == op_kind::to_group) {
                layout_propagation_for_to_group(cur_op);
            } else if (cur_op->get_kind() == op_kind::from_group) {
                layout_propagation_for_from_group(
                        cur_op, p_engine, prm_attr_mgr, reorder_ops);
            } else if (cur_op->get_kind() == impl::op_kind::StaticReshape) {
                layout_propagation_for_reshape(cur_op);
            } else if (cur_op->get_kind() == impl::op_kind::StaticTranspose) {
                layout_propagation_for_transpose(
                        cur_op, p_engine, prm_attr_mgr, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::expand) {
                layout_propagation_for_expand(cur_op);
            } else if (cur_op->get_kind() == op_kind::dnnl_reorder) {
                layout_propagation_for_reorder(cur_op, p_engine, prm_attr_mgr);
            } else if (cur_op->get_kind() == op_kind::squeeze) {
                layout_propagation_for_squeeze(
                        cur_op, p_engine, prm_attr_mgr, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_bn_folding) {
                layout_propagation_for_bn_folding(cur_op, p_engine);
            } else if (cur_op->get_kind() == op_kind::dnnl_resampling) {
                layout_propagation_for_resampling(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_resampling_bwd) {
                layout_propagation_for_resampling_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_sum) {
                layout_propagation_for_dnnl_sum(
                        cur_op, p_engine, prm_attr_mgr, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_binary) {
                layout_propagation_for_binary(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_softmax) {
                layout_propagation_for_softmax(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_softmax_bwd) {
                layout_propagation_for_softmax_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_logsoftmax) {
                layout_propagation_for_logsoftmax(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_logsoftmax_bwd) {
                layout_propagation_for_logsoftmax_bwd(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_shuffle) {
                layout_propagation_for_shuffle(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() == op_kind::dnnl_reduction) {
                layout_propagation_for_reduction(
                        cur_op, p_engine, prm_attr_mgr, pd_cache, reorder_ops);
            } else if (cur_op->get_kind() != op_kind::dnnl_constant_scales
                    && cur_op->get_kind() != op_kind::dnnl_constant_zps) {
                assertm(false,
                        "none layout propagation function for current op");
            }
            return status::success;
        });
    };

    auto need_prop_once_more = [&](const std::shared_ptr<subgraph_t> &sg) {
        const auto &ops = sg->get_ops();
        for (const auto &cur_op : ops) {
            for (const auto &in : cur_op->get_input_values()) {
                if (ltw(in->get_logical_tensor()).layout_type()
                        == layout_type::any) {
                    return true;
                }
            }
            size_t out_idx = 0;
            for (const auto &out : cur_op->get_output_values()) {
                // ignore the second output of conv_depthwise
                if (cur_op->get_kind() == op_kind::dnnl_conv_depthwise
                        && out_idx > 0)
                    continue;
                if (ltw(out->get_logical_tensor()).layout_type()
                        == layout_type::any) {
                    return true;
                }
                out_idx++;
            }
        }
        return false;
    };

    // store those reorders inserted during propagation
    std::vector<op_ptr> reorder_ops;
    do {
        // main layout propgation function
        layout_propagation_func(sg, reorder_ops);
    } while (need_prop_once_more(sg));

    for (const auto &op : reorder_ops)
        subgraph.emplace_back(op);

    remove_optional_conv_dw_output(subgraph, pd_cache);

    // fill layout information for subgraph's inputs
    for (size_t i = 0; i < sg->ins_.size(); i++) {
        for (auto in_val : sg->get_input_values()) {
            auto lt = in_val->get_logical_tensor();
            if (lt.id == sg->ins_[i].id) {
                auto md = make_dnnl_memory_desc(lt);
                fill_layout_info(&(sg->ins_[i]), md);
            }
        }
    }

    // fill layout information for subgraph's outputs
    for (size_t i = 0; i < sg->outs_.size(); i++) {
        for (auto out_val : sg->get_output_values()) {
            auto lt = out_val->get_logical_tensor();
            if (lt.id == sg->outs_[i].id) {
                auto md = make_dnnl_memory_desc(lt);
                fill_layout_info(&(sg->outs_[i]), md);
            }
        }
    }

    return impl::status::success;
}

} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl
