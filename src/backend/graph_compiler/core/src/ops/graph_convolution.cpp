/*******************************************************************************
 * Copyright 2022 Intel Corporation
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
 ******************************************************************************/

#include <numeric>
#include <string>
#include <utility>

#include "compiler/ir/graph/fusible_op.hpp"
#include "convolution.hpp"
#include "graph_convolution.hpp"
#include <util/math_utils.hpp>

namespace sc {
namespace ops {

static void permute_shape_NXC2NCX(sc_dims &shape) {
    size_t ndims = shape.size();
    sc_dim channel = shape[ndims - 1];
    for (size_t i = 1; i < ndims - 1; ++i) {
        shape[i + 1] = shape[i];
    }
    shape[1] = channel;
}

static void permute_shape_NCX2NXC(sc_dims &shape) {
    size_t ndims = shape.size();
    sc_dim channel = shape[1];
    for (size_t i = 1; i < ndims - 1; ++i) {
        shape[i] = shape[i + 1];
    }
    shape[ndims - 1] = channel;
}

static void permute_shape_XIO2OIX(sc_dims &shape) {
    size_t ndims = shape.size();
    sc_dim out_channel = shape[ndims - 1];
    sc_dim in_channel = shape[ndims - 2];
    for (size_t i = 0; i < ndims - 2; ++i) {
        shape[i + 2] = shape[i];
    }
    shape[0] = out_channel;
    shape[1] = in_channel;
}

sc_dims conv_fwd_op_t::infer_out_dims(sc_graph_t &owner_graph,
        const sc_dims &input_dims, const sc_dims &filter_dims,
        const sc_dims &pads_begin, const sc_dims &pads_end,
        const sc_dims &strides, const std::string &data_format,
        const std::string &filter_format, bool is_same) {
    // logic besides conv_fwd_core_op_t::infer_out_dims will not be affected by
    // dynamic shape
    sc_dims output_dims = input_dims;
    int ndims = input_dims.size();
    int channel_axis = data_format == "NCX" ? 1 : ndims - 1;
    int K = filter_format == "XIO" ? filter_dims[ndims - 1] : filter_dims[0];
    output_dims[channel_axis] = K;
    sc_dims input_dims_copy = input_dims;
    sc_dims filter_dims_copy = filter_dims;
    if (!is_same) {
        if (data_format == "NXC") { permute_shape_NXC2NCX(input_dims_copy); }
        if (filter_format == "XIO") { permute_shape_XIO2OIX(filter_dims_copy); }
        // TODO(xxx): fix the logic here
        // the logic here will infer a 1st set of new unknown dim axis in output
        // the conv_fwd_core in get_graph_impl will do the same inferring again
        // which will introduce 2nd set of unknown dim axis
        // needs to add mapping between these 2 set of unknown axis
        output_dims = conv_fwd_core_op_t::infer_out_dims(owner_graph,
                input_dims_copy, filter_dims_copy, pads_begin, pads_end,
                strides);
        if (data_format == "NXC") { permute_shape_NCX2NXC(output_dims); }
    }
    return output_dims;
}

conv_fwd_op_t::conv_fwd_op_t(const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs) {
    COMPILE_ASSERT((ins.size() == 2 || ins.size() == 3),
            "convolution op's inputs size should be 2(input, filter) or "
            "3(input, filter, bias).");
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    attrs_ = attrs;
    op_name_ = "conv_fwd";
    sc_dims input_dims = info_.inputs_[0]->details_.get_plain_dims();
    sc_dims filter_dims = info_.inputs_[1]->details_.get_plain_dims();
    size_t ndims = input_dims.size();
    auto data_format = attrs_.get_or_else("data_format", std::string("NXC"));
    auto filter_format
            = attrs_.get_or_else("filter_format", std::string("XIO"));
    auto strides = attrs_.get<sc_dims>("strides");
    bool is_same = false;
    if (attrs_.has_key("auto_pad")) {
        auto pad_type = attrs_.get<std::string>("auto_pad");
        if (pad_type == "VALID") {
            attrs_.set<sc_dims>("pads_begin", sc_dims(ndims - 2, 0));
            attrs_.set<sc_dims>("pads_end", sc_dims(ndims - 2, 0));
        } else if (pad_type == "SAME_UPPER" || pad_type == "SAME_LOWER") {
            // pass the infer_auto_pad logic here to conv_fwd_core
            is_same = true;
        }
    }
    sc_dims pads_begin, pads_end;
    COMPILE_ASSERT(attrs_.has_key("pads_begin") && attrs_.has_key("pads_end"),
            "pads_begin and pads_end info must be set for convolution op");
    if (!is_same) {
        // if not auto_pad, use pads related attributes
        pads_begin = attrs_.get<sc_dims>("pads_begin");
        pads_end = attrs_.get<sc_dims>("pads_end");
    }
    auto expected_out_shape = infer_out_dims(get_owner_graph(), input_dims,
            filter_dims, pads_begin, pads_end, strides, data_format,
            filter_format, is_same);
    auto out_dtype = info_.inputs_[0]->details_.dtype_;
    if (outs.empty()) {
        info_.outputs_.emplace_back(std::make_shared<graph_tensor>(
                this, sc_data_format_t(), expected_out_shape, out_dtype));
    } else {
        COMPILE_ASSERT(
                info_.outputs_.size() == 1, "convolution expects 1 output.");
        COMPILE_ASSERT(info_.outputs_[0]->details_.get_plain_dims()
                        == expected_out_shape,
                "Bad output shape for convolution");
        COMPILE_ASSERT(info_.outputs_[0]->details_.dtype_ == out_dtype,
                "Bad output dtype for convolution");
    }
}

void conv_fwd_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    // create new input logical tensors
    std::vector<graph_tensor_ptr> inputs, outputs;
    inputs = remake_logical_tensors(info_.inputs_);
    outputs = remake_logical_tensors(info_.outputs_);
    auto ins = graph->make_input(inputs);
    sc_op_ptr conv, graph_out;
    graph_tensor_ptr input = inputs[0], filter = inputs[1];

    bool is_bf16 = inputs[0]->details_.dtype_ == datatypes::bf16;
    if (is_bf16) {
        COMPILE_ASSERT(inputs[1]->details_.dtype_ == datatypes::bf16,
                "Filter shall have the same datatype as input.")
    }
    auto data_format = attrs_.get_or_else("data_format", std::string("NXC"));
    auto filter_format
            = attrs_.get_or_else("filter_format", std::string("XIO"));
    auto dim = inputs[0]->details_.get_plain_dims().size();
    COMPILE_ASSERT(dim == 4 || dim == 5, "Only support conv2D and conv3D.");
    auto is_3D = (dim == 5);

    // insert transpose to make NXC --> NCX and XIO --> OIX
    if (data_format == "NXC") {
        auto permute_input = graph->make("transpose", {input}, {},
                {{"order",
                        is_3D ? std::vector<int> {0, 4, 1, 2, 3}
                              : std::vector<int> {0, 3, 1, 2}}});
        input = permute_input->get_outputs()[0];
    }
    if (filter_format == "XIO") {
        auto permute_weight = graph->make("transpose", {filter}, {},
                {{"order",
                        is_3D ? std::vector<int> {4, 3, 0, 1, 2}
                              : std::vector<int> {3, 2, 0, 1}}});
        filter = permute_weight->get_outputs()[0];
    }

    conv = graph->make("conv_fwd_core", {input, filter}, {}, attrs_);
    if (is_bf16) {
        conv = graph->make(
                "cast", conv->get_outputs(), {}, {{"dtype", datatypes::bf16}});
    }

    if (data_format == "NXC") {
        // conv_fwd_core's output is with NCX plain shape
        // need to permute NCX to NXC
        conv = graph->make("transpose", conv->get_outputs(), {},
                {{"order",
                        is_3D ? std::vector<int> {0, 2, 3, 4, 1}
                              : std::vector<int> {0, 2, 3, 1}}});
    }
    // add bias
    if (info_.inputs_.size() == 3) {
        COMPILE_ASSERT(inputs[2]->details_.get_plain_dims().size() == 1,
                "Convolution op's bias shall be 1D tensor.")
        if (is_bf16) {
            COMPILE_ASSERT(inputs[2]->details_.dtype_ == datatypes::bf16,
                    "Bias should have the same data type as input and filter.")
        }

        int channel_axis = data_format == "NCX" ? 1 : dim - 1;
        COMPILE_ASSERT(
                conv->get_outputs()[0]->details_.get_plain_dims()[channel_axis]
                        == inputs[2]->details_.get_plain_dims()[0],
                "Bias size shall match with channel size.")
        auto bias = graph->make("add", {conv->get_outputs()[0], inputs[2]}, {},
                {{"bc_axis", std::vector<int> {channel_axis}}});
        graph->make_output(bias->get_outputs());
    } else {
        graph->make_output(conv->get_outputs());
    }
}

void conv_fwd_op_t::query_format(context_ptr ctx,
        std::vector<std::vector<format_stride_pair>> &supported_ins,
        std::vector<std::vector<format_stride_pair>> &supported_outs) {}

conv_bwd_data_op_t::conv_bwd_data_op_t(const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs) {
    COMPILE_ASSERT(ins.size() == 2 || ins.size() == 3,
            "conv_bwd_data's inputs size should be 2(output_delta, filter) or "
            "3(output_delta, filter, output_shape).");
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    attrs_ = attrs;
    op_name_ = "conv_bwd_data";
    COMPILE_ASSERT(attrs_.has_key("output_shape"),
            "conv_bwd_data currently does not support reading dynamic shape "
            "passed as one of the input.");
    auto out_shape = attrs_.get<sc_dims>("output_shape");
    auto out_dtype = info_.inputs_[0]->details_.dtype_;
    if (outs.empty()) {
        info_.outputs_.emplace_back(std::make_shared<graph_tensor>(
                this, sc_data_format_t(), out_shape, out_dtype));
    } else {
        COMPILE_ASSERT(
                info_.outputs_.size() == 1, "conv_bwd_data expects 1 output.");
        COMPILE_ASSERT(
                info_.outputs_[0]->details_.get_plain_dims() == out_shape,
                "Bad output shape for conv_bwd_data");
        COMPILE_ASSERT(info_.outputs_[0]->details_.dtype_ == out_dtype,
                "Bad output dtype for conv_bwd_data");
    }
}

void conv_bwd_data_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    // create new input logical tensors
    std::vector<graph_tensor_ptr> inputs, outputs;
    inputs = remake_logical_tensors(info_.inputs_);
    outputs = remake_logical_tensors(info_.outputs_);
    auto ins = graph->make_input(inputs);
    sc_op_ptr conv, graph_out;
    graph_tensor_ptr output_delta = inputs[0], filter = inputs[1];

    bool is_bf16 = inputs[0]->details_.dtype_ == datatypes::bf16;
    auto data_format = attrs_.get_or_else("data_format", std::string("NXC"));
    auto filter_format
            = attrs_.get_or_else("filter_format", std::string("XIO"));
    auto dim = inputs[0]->details_.get_plain_dims().size();
    COMPILE_ASSERT(dim == 4 || dim == 5, "Only support conv2D and conv3D.");
    auto is_3D = (dim == 5);

    // insert transpose to make NXC --> NCX and XIO --> OIX
    if (data_format == "NXC") {
        auto permute_output_delta = graph->make("transpose", {output_delta}, {},
                {{"order",
                        is_3D ? std::vector<int> {0, 4, 1, 2, 3}
                              : std::vector<int> {0, 3, 1, 2}}});
        output_delta = permute_output_delta->get_outputs()[0];

        // change output_shape attributes
        auto output_shape = attrs_.get<sc_dims>("output_shape");
        permute_shape_NXC2NCX(output_shape);
        attrs_.set<sc_dims>("output_shape", output_shape);
    }
    if (filter_format == "XIO") {
        auto permute_weight = graph->make("transpose", {filter}, {},
                {{"order",
                        is_3D ? std::vector<int> {4, 3, 0, 1, 2}
                              : std::vector<int> {3, 2, 0, 1}}});
        filter = permute_weight->get_outputs()[0];
    }

    conv = graph->make(
            "conv_bwd_data_core", {output_delta, filter}, {}, attrs_);
    if (is_bf16) {
        conv = graph->make(
                "cast", conv->get_outputs(), {}, {{"dtype", datatypes::bf16}});
    }

    if (data_format == "NXC") {
        // permute NCX to NXC
        conv = graph->make("transpose", conv->get_outputs(), {},
                {{"order",
                        is_3D ? std::vector<int> {0, 2, 3, 4, 1}
                              : std::vector<int> {0, 2, 3, 1}}});
    }
    graph->make_output(conv->get_outputs());
}

void conv_bwd_data_op_t::query_format(context_ptr ctx,
        std::vector<std::vector<format_stride_pair>> &supported_ins,
        std::vector<std::vector<format_stride_pair>> &supported_outs) {}

conv_bwd_weight_op_t::conv_bwd_weight_op_t(
        const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs) {
    COMPILE_ASSERT(ins.size() == 2 || ins.size() == 3,
            "conv_bwd_weight's inputs size should be 2(input_forward, "
            "output_delta) or 3(input_forward, output_delta, filter_shape).");
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    attrs_ = attrs;
    op_name_ = "conv_bwd_weight";
    COMPILE_ASSERT(attrs_.has_key("filter_shape"),
            "conv_bwd_weight currently does not support reading dynamic shape "
            "passed as one of the input.");
    auto out_shape = attrs_.get<sc_dims>("filter_shape");
    auto out_dtype = info_.inputs_[0]->details_.dtype_;
    if (outs.empty()) {
        info_.outputs_.emplace_back(std::make_shared<graph_tensor>(
                this, sc_data_format_t(), out_shape, out_dtype));
    } else {
        COMPILE_ASSERT(info_.outputs_.size() == 1,
                "conv_bwd_weight expects 1 output.");
        COMPILE_ASSERT(
                info_.outputs_[0]->details_.get_plain_dims() == out_shape,
                "Bad output shape for conv_bwd_weight");
        COMPILE_ASSERT(info_.outputs_[0]->details_.dtype_ == out_dtype,
                "Bad output dtype for conv_bwd_weight");
    }
}

void conv_bwd_weight_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    // create new input logical tensors
    std::vector<graph_tensor_ptr> inputs, outputs;
    inputs = remake_logical_tensors(info_.inputs_);
    outputs = remake_logical_tensors(info_.outputs_);
    auto ins = graph->make_input(inputs);
    sc_op_ptr conv, graph_out;
    graph_tensor_ptr input = inputs[0], output_delta = inputs[1];

    bool is_bf16 = inputs[0]->details_.dtype_ == datatypes::bf16;
    auto data_format = attrs_.get_or_else("data_format", std::string("NXC"));
    auto filter_format
            = attrs_.get_or_else("filter_format", std::string("XIO"));
    auto dim = inputs[0]->details_.get_plain_dims().size();
    COMPILE_ASSERT(dim == 4 || dim == 5, "Only support conv2D and conv3D.");
    auto is_3D = (dim == 5);

    // insert transpose to make NXC --> NCX and XIO --> OIX
    if (data_format == "NXC") {
        auto permute_input = graph->make("transpose", {input}, {},
                {{"order",
                        is_3D ? std::vector<int> {0, 4, 1, 2, 3}
                              : std::vector<int> {0, 3, 1, 2}}});
        input = permute_input->get_outputs()[0];

        auto permute_output_delta = graph->make("transpose", {output_delta}, {},
                {{"order",
                        is_3D ? std::vector<int> {0, 4, 1, 2, 3}
                              : std::vector<int> {0, 3, 1, 2}}});
        output_delta = permute_output_delta->get_outputs()[0];
    }

    if (filter_format == "XIO") {
        // change filter_shape attributes
        auto filter_shape = attrs_.get<sc_dims>("filter_shape");
        permute_shape_XIO2OIX(filter_shape);
        attrs_.set<sc_dims>("filter_shape", filter_shape);
    }

    conv = graph->make(
            "conv_bwd_weight_core", {input, output_delta}, {}, attrs_);

    if (is_bf16) {
        conv = graph->make(
                "cast", conv->get_outputs(), {}, {{"dtype", datatypes::bf16}});
    }
    // insert transpose for output: OIX to XIO
    if (filter_format == "XIO") {
        conv = graph->make("transpose", conv->get_outputs(), {},
                {{"order",
                        is_3D ? std::vector<int> {2, 3, 4, 1, 0}
                              : std::vector<int> {2, 3, 1, 0}}});
    }
    graph->make_output(conv->get_outputs());
}

void conv_bwd_weight_op_t::query_format(context_ptr ctx,
        std::vector<std::vector<format_stride_pair>> &supported_ins,
        std::vector<std::vector<format_stride_pair>> &supported_outs) {}

} // namespace ops

OP_REGISTER(::sc::ops::conv_fwd_op_t, conv_fwd)
OP_REGISTER(::sc::ops::conv_bwd_data_op_t, conv_bwd_data)
OP_REGISTER(::sc::ops::conv_bwd_weight_op_t, conv_bwd_weight)
} // namespace sc
