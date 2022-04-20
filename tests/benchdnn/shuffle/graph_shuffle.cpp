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

#include "oneapi/dnnl/dnnl_graph.hpp"

#include "dnnl_graph_common.hpp"
#include "utils/compare.hpp"

#include "shuffle/graph_shuffle.hpp"
#include "shuffle/shuffle.hpp"

namespace benchdnnext {
namespace shuffle {

void check_known_skipped_case_graph(const ::shuffle::prb_t *prb, res_t *res) {
    // TODO: to align with original benchdnn, we should consider moving
    // skip_unimplemented_prb call after compilation step
    skip_invalid_and_unimplemented_prb(prb, res);
}

fill_status_t shuffle_graph_prb_t::handle_main_op_(
        const ::shuffle::prb_t *prb) {
    const auto add_reshape = [this](const size_t id,
                                     const dnnl::graph::logical_tensor &src,
                                     const dnnl::graph::logical_tensor &dst) {
        dnnl::graph::op reshape(id, dnnl::graph::op::kind::StaticReshape, {src},
                {dst}, "reshape");
        reshape.set_attr("shape", dst.get_dims())
                .set_attr("special_zero", false);
        ops_.emplace_back(reshape);
    };

    const auto data_type = convert_dt(prb->dt);
    const int64_t axis = prb->axis;
    const std::string tag = prb->tag;
    int64_t group;
    if (prb->dir & FLAG_FWD) {
        group = prb->group;
    } else {
        // example (axis = 1)
        //
        //     | (4, '8', 4, 4)
        //  reshape
        //     | (4, '2', '4', 4, 4)
        // transpose
        //     | (4, '4', '2', 4, 4)
        //  reshape
        //     | (4, '8', 4, 4)
        //
        // If we look at this pattern from up to bottom, then groups_fwd = 4,
        // from bottom to up however, groups_bwd = 2 = channel_dim / groups_fwd
        group = prb->dims[axis] / prb->group;
    }

    // reshape0
    const auto reshape0_src_dims = prb->dims;
    // - dst dims should be same as src dims except at 'channel' axis
    // - 'channel' value should be replaced with (C / g, g),
    // therefore shape attr will have one more dimension than the src dims.
    auto reshape0_dst_dims = prb->dims;
    reshape0_dst_dims[axis] /= group;
    reshape0_dst_dims.insert(reshape0_dst_dims.begin() + axis + 1, group);

    // transpose
    auto transpose_dst_dims = reshape0_dst_dims;
    std::swap(transpose_dst_dims[axis], transpose_dst_dims[axis + 1]);
    // After reshape, we have to make a transposition of g and C / g.
    // To do that we have to do following steps:
    // - fill the order with n consecutive values, where n = input size - 1
    // - swap indices of 'channel' axis with successor axis
    std::vector<int64_t> transpose_order(transpose_dst_dims.size());
    std::iota(transpose_order.begin(), transpose_order.end(), 0);
    std::swap(transpose_order[axis], transpose_order[axis + 1]);

    // reshape1
    // input and output dims of the whole pattern must equal
    const auto reshape1_dst_dims = prb->dims;

    const auto reshape0_id = ops_.size();
    const std::string RESHAPE0_ID_STR = std::to_string(reshape0_id);
    tensor_id["reshape" + std::to_string(reshape0_id)].push_back(
            RESHAPE0_ID_STR);
    const std::string RESHAPE0_SRC {RESHAPE0_ID_STR + "_SRC"};
    const std::string RESHAPE0_DST {RESHAPE0_ID_STR + "_DST"};
    tensor_descs_.emplace(RESHAPE0_SRC, data_type, reshape0_src_dims, tag);
    tensor_descs_.emplace(RESHAPE0_DST, data_type, reshape0_dst_dims, tag);

    add_reshape(reshape0_id, tensor_descs_[RESHAPE0_SRC],
            tensor_descs_[RESHAPE0_DST]);

    const auto transpose_id = ops_.size();
    const std::string TRANSPOSE_ID_STR = std::to_string(transpose_id);
    tensor_id["transpose"].push_back(TRANSPOSE_ID_STR);
    const std::string TRANSPOSE_DST {TRANSPOSE_ID_STR + "_DST"};
    tensor_descs_.emplace(TRANSPOSE_DST, data_type, transpose_dst_dims, tag);

    dnnl::graph::op transpose(transpose_id,
            dnnl::graph::op::kind::StaticTranspose,
            {tensor_descs_[RESHAPE0_DST]}, {tensor_descs_[TRANSPOSE_DST]},
            "transpose");
    transpose.set_attr("order", transpose_order);
    ops_.emplace_back(transpose);

    const auto reshape1_id = ops_.size();
    const std::string RESHAPE1_ID_STR = std::to_string(reshape1_id);
    tensor_id["reshape" + std::to_string(reshape1_id)].push_back(
            RESHAPE1_ID_STR);
    const std::string RESHAPE1_DST {RESHAPE1_ID_STR + "_DST"};
    tensor_descs_.emplace(RESHAPE1_DST, data_type, reshape1_dst_dims, tag);

    add_reshape(reshape1_id, tensor_descs_[TRANSPOSE_DST],
            tensor_descs_[RESHAPE1_DST]);

    curr_out_map_ids_.assign({RESHAPE1_ID_STR});

    return fill_status::DONE;
}

// In oneDNN Graph we use a specific
// combination of Reshape -> Transpose -> Reshape chain,
// to describe a single Shuffle op.
// See @example cpu_shuffle_pattern_f32.cpp for more information.
int doit(const ::shuffle::prb_t *prb, res_t *res) {
    res->impl_name = "graph";
    if (bench_mode == LIST) return res->state = LISTED, OK;

    check_known_skipped_case_graph(prb, res);
    if (res->state == SKIPPED) return OK;

    shuffle_graph_prb_t graph_prb(prb);
    if (graph_prb.ctor_status != fill_status::DONE
            && graph_prb.ctor_status != fill_status::UNHANDLED_CONFIG_OPTIONS) {
        return res->state = UNIMPLEMENTED, FAIL;
    }

    auto graph_h = graph_prb.to_graph();

    // Filter partitions
    const auto partitions = graph_h.get_partitions();
    if (partitions.empty() || partitions.size() > 1)
        return res->state = FAILED, FAIL;

    const auto par = partitions[0];
    if (!par.is_supported()) return res->state = UNIMPLEMENTED, FAIL;

    const auto ins = par.get_in_ports();
    const auto outs = par.get_out_ports();

    auto cp = compile_partition(::shuffle::init_pd, prb, res, par, ins, outs);

    auto src_fp = make_dnn_mem(ins[0], dt::f32, tag::abx);
    auto dst_fp = make_dnn_mem(outs[0], dt::f32, tag::abx);

    auto src_dt = make_dnn_mem(ins[0], prb->tag);
    auto dst_dt = make_dnn_mem(outs[0], prb->tag);

    SAFE(::shuffle::fill_src(prb, src_dt, src_fp), WARN);

    const dnnl::graph::engine &eng = get_test_engine();

    dnnl::graph::tensor src_tensor(ins[0], eng, static_cast<void *>(src_dt));
    dnnl::graph::tensor dst_tensor(outs[0], eng, static_cast<void *>(dst_dt));

    std::vector<dnnl::graph::tensor> tensors_in {src_tensor};
    std::vector<dnnl::graph::tensor> tensors_out {dst_tensor};

    SAFE(execute_and_wait(cp, tensors_in, tensors_out, res), WARN);

    if (is_bench_mode(CORR)) {
        args_t args, ref_args;

        if (prb->dir & FLAG_FWD) {
            args.set(DNNL_ARG_DST, dst_dt);
            ref_args.set(DNNL_ARG_SRC, src_fp);
            ref_args.set(DNNL_ARG_DST, dst_fp);

            check_correctness(
                    prb, {DST}, args, ref_args, ::shuffle::setup_cmp, res);
        } else if (prb->dir & FLAG_BWD) {
            args.set(DNNL_ARG_DIFF_SRC, dst_dt);
            ref_args.set(DNNL_ARG_DIFF_DST, src_fp);
            ref_args.set(DNNL_ARG_DIFF_SRC, dst_fp);

            check_correctness(
                    prb, {SRC}, args, ref_args, ::shuffle::setup_cmp, res);
        } else {
            SAFE(FAIL, CRIT);
        }
    }

    SAFE(measure_perf(res->timer_map.perf_timer(), cp, tensors_in, tensors_out),
            WARN);

    return OK;
}

} // namespace shuffle
} // namespace benchdnnext
