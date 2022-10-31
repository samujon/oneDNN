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

#include "fused_op.hpp"
#include <algorithm>
#include <atomic>
#include <utility>
#include "fusible_op_utils.hpp"
#include "fusion_mgr.hpp"
#include "lowering.hpp"
#include "outer_loop_generator.hpp"
#include "pass/pass.hpp"
#include "tunable_op.hpp"
#include "visitor.hpp"
#include <compiler/ir/builder.hpp>
#include <compiler/ir/graph/dynamic_dispatch_key.hpp>
#include <compiler/ir/graph/dynamic_utils.hpp>
#include <compiler/ir/graph/lowering.hpp>
#include <compiler/ir/graph/mixed_partition.hpp>
#include <compiler/ir/graph/transform/transform.hpp>
#include <compiler/ir/graph/utils.hpp>
#include <compiler/ir/transform/constant_fold.hpp>
#include <compiler/ir/transform/dead_write_eliminate.hpp>
#include <compiler/ir/transform/dyn_tsr_transform.hpp>
#include <compiler/ir/transform/func_inline.hpp>
#include <compiler/ir/transform/loop_transform.hpp>
#include <compiler/ir/transform/scope_flatten.hpp>
#include <compiler/ir/transform/tensor2var.hpp>
#include <compiler/ir/transform/tensor_shrink.hpp>
#include <ops/fusible/binary_elemwise.hpp>
#include <ops/fusible/memory_movement.hpp>
#include <ops/fusible/reduce.hpp>
#include <ops/fusible/unary_elemwise.hpp>
#include <ops/matmul_core.hpp>
#include <runtime/config.hpp>
#include <runtime/dynamic_dispatch/dynamic_tensor.hpp>
#include <unordered_map>
#include <unordered_set>

SC_MODULE(graph.fused_op)

namespace sc {

fusion_partition_t *fusion_partition_t::get_root() const {
    if (merged_to) {
        assert(ops.empty());
        return merged_to->get_root();
    }
    return const_cast<fusion_partition_t *>(this);
}

bool fusion_partition_t::is_ok_to_add(
        sc_op *op, const op_dep_matrix_t &g) const {
    if (merged_to) { return get_root()->is_ok_to_add(op, g); }
    for (auto &in : op->get_inputs()) {
        // if the input is in the partition, don't need to check
        if (contains(in->producer_owner_)) { continue; }
        // if "in" depends on an op in the partition
        if (main_tunable_op
                && g.lookup(main_tunable_op->logical_op_id_,
                           in->producer_owner_->logical_op_id_)
                        == 1) {
            return false;
        }
        for (auto &op_in_set : ops) {
            auto result = g.lookup(op_in_set->logical_op_id_,
                    in->producer_owner_->logical_op_id_);
            // if "in" depends on an op in the partition
            if (result == 1) { return false; }
        }
    }
    return true;
}

bool fusion_partition_t::contains(sc_op *op) const {
    if (merged_to) { return get_root()->contains(op); }
    return op == main_tunable_op.get()
            || ops.find(op->shared_from_this()) != ops.end();
}

// merge the ops in "other" to "this"
void fusion_partition_t::merge(const ptr &other) const {
    auto ths_root = get_root();
    auto other_root = other->get_root();
    if (ths_root == other_root) { return; }
    ths_root->ops.insert(other_root->ops.begin(), other_root->ops.end());
    assert(other_root->main_tunable_op == nullptr);
    other_root->ops.clear();
    other_root->merged_to = ths_root->shared_from_this();
}

namespace graph {
void mark_read_or_write_buffers(std::vector<expr> &args, bool is_read) {
    const char *name = is_read ? "read_buffer" : "write_buffer";
    for (auto &tsr : args) {
        tsr->attr()[name] = true;
    }
}

func_t create_func_decl_for_op(
        sc_op *op, std::vector<expr> &ins, std::vector<expr> &outs) {
    auto &graph = op->get_owner_graph();
    ins = ins.empty() ? graph::tensor_detail_to_ir_tensor(
                  graph, "__ins_", op->get_inputs())
                      : ins;
    outs = outs.empty() ? graph::tensor_detail_to_ir_tensor(
                   graph, "__outs_", op->get_outputs())
                        : outs;
    graph::mark_read_or_write_buffers(ins, true);
    graph::mark_read_or_write_buffers(outs, false);
    std::vector<expr> args = outs;
    args.insert(args.end(), ins.begin(), ins.end());
    std::string func_name;
    if (auto layer_name
            = op->attrs_.get_or_null<std::string>(op_attr_key::layer_name)) {
        COMPILE_ASSERT(!layer_name->empty() && isalpha(layer_name->front()),
                "Bad layername: " << *layer_name);
        func_name = *layer_name;
    } else {
        func_name = op->op_name_;
    }
    func_name += "__";
    func_name += std::to_string(op->logical_op_id_);
    auto func = builder::make_func(func_name, args,
            make_stmt<stmts_node_t>(std::vector<stmt>()), datatypes::boolean);
    // func->attr()["Gflop"] = gen_ptr->get_gflop();
    return func;
}

func_t create_query_func_decl_for_op(sc_op *op, std::vector<expr> &ins,
        std::vector<expr> &ori_ins, std::vector<expr> &outs,
        std::vector<expr> &in_fmts, std::vector<expr> &ori_in_fmts,
        std::vector<expr> &out_fmts, std::vector<expr> &out_sizes,
        expr &kernel) {
    func_t func = create_func_decl_for_op(op, ins, outs);
    in_fmts.resize(ins.size());
    out_fmts.resize(outs.size());
    out_sizes.resize(outs.size());
    sc_op *tunable_op = nullptr;
    assert(op->isa<fused_op_t>());
    if (!op->stc_cast<fused_op_t>()->main_op_.empty()) {
        tunable_op = op->stc_cast<fused_op_t>()->main_op_.ops_[1].get();
        auto sz = tunable_op->get_inputs().size();
        auto &graph = op->get_owner_graph();
        for (size_t i = 0; i < sz; i++) {
            auto ori_in = graph::tensor_detail_to_ir_tensor(graph,
                    std::string("__ori_ins_") + std::to_string(i),
                    op->get_inputs()[i]->details_);
            ori_in->attr().set(attr_keys::always_trans, true);
            ori_ins.emplace_back(ori_in);
            ori_in_fmts.emplace_back(builder::make_tensor(
                    ori_in.static_as<tensor>()->name_ + "_format",
                    {UINT64_C(1)}, datatypes::index));
        }
    }
    std::transform(ins.begin(), ins.end(), in_fmts.begin(), [](const expr &in) {
        in->attr().set(attr_keys::always_trans, true);
        return builder::make_tensor(in.static_as<tensor>()->name_ + "_format",
                {UINT64_C(1)}, datatypes::index);
    });
    std::transform(
            outs.begin(), outs.end(), out_fmts.begin(), [](const expr &in) {
                in->attr().set(attr_keys::always_trans, true);
                return builder::make_tensor(
                        in.static_as<tensor>()->name_ + "_format",
                        {UINT64_C(1)}, datatypes::index);
            });
    std::transform(
            outs.begin(), outs.end(), out_sizes.begin(), [](const expr &in) {
                return builder::make_tensor(
                        in.static_as<tensor>()->name_ + "_size", {UINT64_C(1)},
                        datatypes::index);
            });
    // table should be get from module global data, here is only for the
    // alignment to other functions.
    auto table = builder::make_var(datatypes::pointer, "dummy_table");
    std::vector<expr> args = func->params_;
    args.insert(args.begin(), table);
    args.insert(args.end(), ori_ins.begin(), ori_ins.end());
    args.insert(args.end(), out_fmts.begin(), out_fmts.end());
    args.insert(args.end(), in_fmts.begin(), in_fmts.end());
    args.insert(args.end(), ori_in_fmts.begin(), ori_in_fmts.end());
    args.insert(args.end(), out_sizes.begin(), out_sizes.end());
    kernel = builder::make_var(datatypes::pointer, "func_kernel");
    args.push_back(kernel);
    func->params_ = args;
    func->name_ = std::string("query_format_") + func->name_;
    return func;
}
} // namespace graph
static std::atomic<int> out_idx(0);

void collect_shrinked_graph_lt_map(
        const sc_graph_t &graph, gt2gt_map &lt_map, int shrink_size) {
    op_visitor_t pre_vis = op_visitor_t::bfs();
    pre_vis.visit_graph(graph, [&](const sc_op_ptr &op) {
        if (op->isa<input_op>() || op->isa<output_op>()
                || op->isa<constant_op_t>()) {
            return;
        } else if (auto bw_op
                = op->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            bw_op->collect_shrinked_lt_map(shrink_size, lt_map);
        } else {
            COMPILE_ASSERT(0, "Unexpected op kind found: " << op->op_name_)
        }
    });
}

void collect_shrinked_graph_axes_map(
        const sc_graph_t &graph, gt2axes_map &axes_map, int shrink_size) {
    op_visitor_t pre_vis = op_visitor_t::bfs();
    pre_vis.visit_graph(graph, [&](const sc_op_ptr &op) {
        if (op->isa<input_op>() || op->isa<output_op>()
                || op->isa<constant_op_t>()) {
            return;
        } else if (auto bw_op
                = op->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            bw_op->collect_shrinked_axes_map(shrink_size, axes_map);
        } else {
            COMPILE_ASSERT(0, "Unexpected op kind found: " << op->op_name_)
        }
    });
}

// shrink graph by shrink size
sc_graph_t shrink_graph(const sc_graph_t &graph, gt2gt_map &lt_map) {
    sc_graph_t shrinked_graph;
    shrinked_graph.sync_dynamic_info_with_graph(graph);
    op_visitor_t vis(op_visitor_t::dequeue_selector,
            op_visitor_t::create_DAG_updater(graph.ops_.size()));
    std::unordered_map<sc_op_ptr, int> op_id_map;
    vis.visit_graph(graph, [&](const sc_op_ptr &node) {
        sc_op_ptr new_node;
        if (node->dyn_cast<input_op>()) {
            new_node = shrinked_graph.make_input(
                    {lt_map.get(node->get_outputs()[0])});
            new_node->attrs_ = node->attrs_;
        } else if (node->dyn_cast<output_op>()) {
            new_node = new_node = shrinked_graph.make_output(
                    {lt_map.get(node->get_inputs()[0])});
            new_node->attrs_ = node->attrs_;
        } else if (node->dyn_cast<constant_op_t>()) {
            new_node = node->dyn_cast<op_traits::copyable_t>()->copy(
                    {}, {lt_map.get(node->get_outputs()[0])}, shrinked_graph);
        } else if (auto bw_node
                = node->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            new_node = bw_node->bw_shrinked_copy(lt_map, shrinked_graph);
        }
        op_id_map[new_node] = node->logical_op_id_;
    });
    shrinked_graph.attrs_ = graph.attrs_;
    shrinked_graph.resort_op_ids(op_id_map);
    return shrinked_graph;
}

fusion_mgr_ptr shrink_fmgr(const fusion_mgr_ptr &fmgr, gt2gt_map &lt_map) {
    auto &graph = fmgr->get_graph();
    auto new_fmgr = std::make_shared<fusion_manager>();
    op_visitor_t vis(op_visitor_t::dequeue_selector,
            op_visitor_t::create_DAG_updater(graph.ops_.size()));
    vis.visit_graph(graph, [&](const sc_op_ptr &node) {
        sc_op_ptr new_node;
        if (node->dyn_cast<input_op>()) {
            new_node = new_fmgr->make_input(
                    {lt_map.get(node->get_outputs()[0])});
            new_node->attrs_ = node->attrs_;
        } else if (node->dyn_cast<output_op>()) {
            const auto &outtsr = lt_map.get(node->get_inputs()[0]);
            new_node = new_node = new_fmgr->make<output_op>(outtsr);
            new_node->attrs_ = node->attrs_;
        } else if (node->dyn_cast<constant_op_t>()) {
            new_node = node->dyn_cast<op_traits::copyable_t>()->copy({},
                    {lt_map.get(node->get_outputs()[0])},
                    new_fmgr->get_graph());
        } else if (auto bw_node
                = node->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            new_node = bw_node->bw_shrinked_copy(lt_map, new_fmgr->get_graph());
        }
    });
    new_fmgr->get_graph().attrs_ = graph.attrs_;
    return new_fmgr;
}

op_traits::post_fusion_acceptable_t *fused_op_t::get_main_op() const {
    COMPILE_ASSERT(main_op_.ops_.size() == 2, "Bad internal graph");
    auto op = main_op_.ops_[1]->dyn_cast<op_traits::post_fusion_acceptable_t>();
    COMPILE_ASSERT(op, "The main op is not post_fusion_acceptable_t");
    return op;
}

bool fused_op_t::is_valid(const context_ptr &ctx) {
    if (main_op_.empty()) { return true; }
    return main_op_.ops_[1]->is_valid(ctx);
}

void fused_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    throw std::runtime_error("fused_op_t::get_graph Not implemented");
}

sc_op_ptr find_first_dispatch_op(const std::vector<sc_op_ptr> &ops) {
    for (auto &op : ops) {
        if (can_op_be_dispatched(op)) { return op; }
    }
    return nullptr;
}

fused_op_t::fused_op_t(const std::string &name, sc_graph_t &&main_op,
        std::shared_ptr<fusion_manager> fuse_mgr,
        const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs)
    : mgr_(std::move(fuse_mgr)), main_op_(std::move(main_op)) {
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    attrs_ = attrs;
    if (!main_op_.ops_.empty()) {
        attrs_.set("horizontal_merge",
                main_op_.ops_[1]->attrs_.get_or_else(
                        "horizontal_merge", horizontal_merge_type::no_merge));
        if (is_dynamic()) { main_dispatch_op_ = main_op_.ops_[1]; }
        main_op_.ops_[0]->set_owner_graph(&main_op_);
        main_op_.ops_[1]->set_owner_graph(&main_op_);
    } else {
        auto &ops = mgr_->get_graph().ops_;
        if (is_dynamic()) { main_dispatch_op_ = find_first_dispatch_op(ops); }
    }
    op_name_ = name;
}

sc_op_ptr fused_op_t::copy(const std::vector<graph_tensor_ptr> &ins, // NOLINT
        const std::vector<graph_tensor_ptr> &outs, sc_graph_t &graph) {
    sc_graph_t new_main_op;
    fusion_mgr_ptr new_fmgr;
    if (!main_op_.ops_.empty()) {
        auto base_op = dynamic_cast<sc_op *>(get_main_op());
        auto copyable = base_op->dyn_cast<op_traits::copyable_t>();
        COMPILE_ASSERT(copyable, base_op->op_name_ << " is not copyable");

        auto dummy_inop = new_main_op.make_input(
                copy_logical_tsr(base_op->get_inputs()));
        auto copied = copyable->copy(dummy_inop->get_outputs(),
                copy_logical_tsr(base_op->get_outputs()), new_main_op);
        assert(new_main_op.ops_.size() == 2);
    }
    if (mgr_) new_fmgr = mgr_->copy();
    auto new_fused_op = std::make_shared<fused_op_t>(op_name_,
            std::move(new_main_op), new_fmgr,
            /*ins*/ ins,
            /*outs*/
            outs, attrs_);
    graph.add(new_fused_op);
    return new_fused_op;
}

static sc_dims get_common_dims(const sc_dims &A, const sc_dims &B) {
    sc_dims ret;
    size_t common_size = std::min(A.size(), B.size());
    for (size_t i = 0; i < common_size; i++) {
        if (A[i] != B[i]) return ret;
        ret.emplace_back(A[i]);
    }
    return ret;
}

sc_dims fused_op_t::get_bwise_fuse_shrink_dims() {
    sc_dims bw_dims;
    if (main_op_.ops_.empty()) {
        // outer_loop_generator
        auto base_inp = const_cast<sc_op *>(mgr_->get_first_input())
                                ->dyn_cast<fusible_op_t>();
        for (auto &user : base_inp->get_outputs()[0]->uses_) {
            if (auto bw_op
                    = user.second
                              ->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
                bw_dims = bw_op->get_bwise_fuse_shrink_dims();
                break;
            }
        }
    } else {
        auto base_op = dynamic_cast<sc_op *>(get_main_op());
        if (auto bw_op
                = base_op->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            bw_dims = bw_op->get_bwise_fuse_shrink_dims();
        }
    }
    // double check fused graph
    sc_dims common_bw_dims = bw_dims;
    sc_dims common_no_strided_dims_pre, common_no_strided_dims_post,
            common_no_strided_dims;
    bool no_strided_dims_pre_init = false, no_strided_dims_post_init = false;
    for (auto &op : mgr_->get_graph().ops_) {
        if (common_bw_dims.empty()) break;
        if (op->isa<input_op>() || op->isa<output_op>()
                || op->isa<constant_op_t>())
            continue;
        else if (auto bw_op
                = op->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            auto cur_bw_dims = bw_op->get_bwise_fuse_shrink_dims();
            COMPILE_ASSERT(
                    !op->attrs_.has_key(op_attr_key::bwise_no_strided_dims)
                            || (op->attrs_.get_or_else(
                                        op_attr_key::bwise_break_pre_fuse,
                                        false)
                                    || op->attrs_.get_or_else(
                                            op_attr_key::bwise_break_post_fuse,
                                            false)),
                    "If the op has been set batch-wise no stride dims, it "
                    "means it "
                    "may cause bwise break fusion");
            if (op->isa<reorder_op_t>()
                    && op->attrs_.has_key(op_attr_key::bwise_no_strided_dims)) {
                sc_dims no_strided_dims = op->attrs_.get<sc_dims>(
                        op_attr_key::bwise_no_strided_dims);
                if (op->attrs_.get_or_else(
                            op_attr_key::bwise_break_post_fuse, false)) {
                    if (op->is_single_output_single_use()
                            && op->get_outputs()[0]
                                       ->uses_[0]
                                       .second->isa<output_op>()) {
                        attrs_.set(op_attr_key::bwise_break_post_fuse, true);
                    } else {
                        cur_bw_dims = no_strided_dims;
                    }
                    // cache common no strided dims for fall-back
                    if (!no_strided_dims_post_init) {
                        common_no_strided_dims_post = no_strided_dims;
                        no_strided_dims_post_init = true;
                    } else {
                        common_no_strided_dims_post = get_common_dims(
                                common_no_strided_dims_post, no_strided_dims);
                    }
                }
                if (op->attrs_.get_or_else(
                            op_attr_key::bwise_break_pre_fuse, false)) {
                    if (op->get_inputs()[0]->producer_owner_->isa<input_op>()) {
                        attrs_.set(op_attr_key::bwise_break_pre_fuse, true);
                    } else {
                        cur_bw_dims = no_strided_dims;
                    }
                    // cache common no strided dims for fall-back
                    if (!no_strided_dims_pre_init) {
                        common_no_strided_dims_pre = no_strided_dims;
                        no_strided_dims_pre_init = true;
                    } else {
                        common_no_strided_dims_pre = get_common_dims(
                                common_no_strided_dims_pre, no_strided_dims);
                    }
                }
                op->attrs_.remove(op_attr_key::bwise_no_strided_dims);
                if (op->attrs_.has_key(op_attr_key::bwise_break_pre_fuse))
                    op->attrs_.remove(op_attr_key::bwise_break_pre_fuse);
                if (op->attrs_.has_key(op_attr_key::bwise_break_post_fuse))
                    op->attrs_.remove(op_attr_key::bwise_break_post_fuse);
            }
            common_bw_dims = get_common_dims(common_bw_dims, cur_bw_dims);
        } else {
            common_bw_dims = {};
        }
    }
    // double-check bwise break fuse attr
    if (attrs_.get_or_else(op_attr_key::bwise_break_pre_fuse, false)
            && common_bw_dims.size() <= common_no_strided_dims_pre.size()) {
        if (attrs_.has_key(op_attr_key::bwise_break_pre_fuse)) {
            attrs_.remove(op_attr_key::bwise_break_pre_fuse);
        }
    }
    if (attrs_.get_or_else(op_attr_key::bwise_break_post_fuse, false)
            && common_bw_dims.size() <= common_no_strided_dims_post.size()) {
        if (attrs_.has_key(op_attr_key::bwise_break_post_fuse)) {
            attrs_.remove(op_attr_key::bwise_break_post_fuse);
        }
    }
    // set bwise_no_strided_dims if necessary
    if (attrs_.get_or_else(op_attr_key::bwise_break_pre_fuse, false)
            || attrs_.get_or_else(op_attr_key::bwise_break_post_fuse, false)) {
        COMPILE_ASSERT(no_strided_dims_pre_init || no_strided_dims_post_init,
                "pre/post no strided dims should be set")
        if (no_strided_dims_pre_init && no_strided_dims_post_init) {
            common_no_strided_dims = get_common_dims(
                    common_no_strided_dims_pre, common_no_strided_dims_post);
            common_no_strided_dims
                    = get_common_dims(common_bw_dims, common_no_strided_dims);
        } else if (no_strided_dims_pre_init) {
            common_no_strided_dims = get_common_dims(
                    common_bw_dims, common_no_strided_dims_pre);
        } else {
            common_no_strided_dims = get_common_dims(
                    common_bw_dims, common_no_strided_dims_post);
        }
        attrs_.set(op_attr_key::bwise_no_strided_dims, common_no_strided_dims);
    }
    return common_bw_dims;
}

sc_op_ptr fused_op_t::bw_shrinked_copy(
        gt2gt_map &bw_lt_map, sc_graph_t &shrinked_graph) {
    sc_graph_t new_main_op;
    fusion_mgr_ptr new_fmgr;
    auto ths = this;
    auto shrink_logical_tsr
            = [&bw_lt_map, &ths](const std::vector<graph_tensor_ptr> &old_gt) {
                  std::vector<graph_tensor_ptr> new_gt(old_gt.size());
                  std::transform(old_gt.begin(), old_gt.end(), new_gt.begin(),
                          [&bw_lt_map, &ths](const graph_tensor_ptr &gt) {
                              COMPILE_ASSERT(bw_lt_map.haskey(gt),
                                      ths->op_name_
                                              << ": new input graph tensor not "
                                                 "found in map");
                              return bw_lt_map.get(gt);
                          });
                  return new_gt;
              };
    if (!main_op_.ops_.empty()) {
        auto base_op = dynamic_cast<sc_op *>(get_main_op());
        COMPILE_ASSERT(base_op->isa<op_traits::batchwise_shrinkable_t>(),
                "Please check whether " << base_op->op_name_
                                        << " is the batchwise shrinkable op")
        auto dummy_inop = new_main_op.make_input(
                shrink_logical_tsr(base_op->get_inputs()));
        auto bw_op = base_op->dyn_cast<op_traits::batchwise_shrinkable_t>();
        auto new_base_op = bw_op->bw_shrinked_copy(bw_lt_map, new_main_op);

        assert(new_main_op.ops_.size() == 2);
    }
    new_fmgr = shrink_fmgr(mgr_, bw_lt_map);

    auto old_ins = ths->get_inputs(), old_out = ths->get_outputs();
    auto new_fused_ins = shrink_logical_tsr(old_ins),
         new_fused_out = shrink_logical_tsr(old_out);

    auto new_fused_op = std::make_shared<fused_op_t>(op_name_,
            std::move(new_main_op), new_fmgr,
            /*ins*/ new_fused_ins,
            /*outs*/
            new_fused_out, attrs_);
    shrinked_graph.add(new_fused_op);
    return new_fused_op;
}

void fused_op_t::collect_shrinked_lt_map(int bw_size, gt2gt_map &bw_lt_map) {
    std::vector<graph_tensor_ptr> fused_ins = get_inputs(),
                                  fused_out = get_outputs();
    size_t base_op_out_size = 0;
    if (!main_op_.ops_.empty()) {
        auto base_op = dynamic_cast<sc_op *>(get_main_op());
        COMPILE_ASSERT(base_op->isa<op_traits::batchwise_shrinkable_t>(),
                "Please check whether " << base_op->op_name_
                                        << " is the batchwise shrinkable op")
        if (auto bw_op
                = base_op->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            bw_op->collect_shrinked_lt_map(bw_size, bw_lt_map);
        }
        auto base_ins = base_op->get_inputs();
        auto base_out = base_op->get_outputs();
        base_op_out_size += base_out.size();
        for (size_t i = 0; i < base_ins.size(); i++) {
            COMPILE_ASSERT(
                    bw_lt_map.haskey(base_ins[i]), "Unexpected cases found");
            auto &plain_dims
                    = bw_lt_map.get(base_ins[i])->details_.get_plain_dims();
            op_traits::batchwise_shrinkable_t::record_shrinked_gt(
                    bw_lt_map, fused_ins[i], plain_dims);
        }
    }
    // collect fusion graph
    collect_shrinked_graph_lt_map(mgr_->get_graph(), bw_lt_map, bw_size);

    // collect fused_op self
    auto fmgr_inop = mgr_->get_graph().get_input_ops();
    auto fmgr_outop = mgr_->get_graph().get_output_ops();
    std::vector<graph_tensor_ptr> fmgr_ins(fmgr_inop.size()),
            fmgr_out(fmgr_outop.size());
    std::transform(fmgr_inop.begin(), fmgr_inop.end(), fmgr_ins.begin(),
            [&](const sc_op_ptr &op) { return op->get_outputs()[0]; });
    std::transform(fmgr_outop.begin(), fmgr_outop.end(), fmgr_out.begin(),
            [&](const sc_op_ptr &op) { return op->get_inputs()[0]; });
    for (size_t i = 0; i < fmgr_ins.size(); i++) {
        COMPILE_ASSERT(bw_lt_map.haskey(fmgr_ins[i]), "Unexpected cases found");
        auto &plain_dims
                = bw_lt_map.get(fmgr_ins[i])->details_.get_plain_dims();
        op_traits::batchwise_shrinkable_t::record_shrinked_gt(
                bw_lt_map, fused_ins[i + base_op_out_size], plain_dims);
    }
    for (size_t i = 0; i < fmgr_out.size(); i++) {
        COMPILE_ASSERT(bw_lt_map.haskey(fmgr_out[i]), "Unexpected cases found");
        auto &plain_dims
                = bw_lt_map.get(fmgr_out[i])->details_.get_plain_dims();
        op_traits::batchwise_shrinkable_t::record_shrinked_gt(
                bw_lt_map, fused_out[i], plain_dims);
    }
}

void fused_op_t::collect_shrinked_axes_map(
        int bw_size, gt2axes_map &bw_axes_map) {
    std::vector<graph_tensor_ptr> fused_ins = get_inputs(),
                                  fused_out = get_outputs();
    size_t base_op_out_size = 0;
    if (!main_op_.ops_.empty()) {
        auto base_op = dynamic_cast<sc_op *>(get_main_op());
        COMPILE_ASSERT(base_op->isa<op_traits::batchwise_shrinkable_t>(),
                "Please check whether " << base_op->op_name_
                                        << " is the batchwise shrinkable op")
        if (auto bw_op
                = base_op->dyn_cast<op_traits::batchwise_shrinkable_t>()) {
            bw_op->collect_shrinked_axes_map(bw_size, bw_axes_map);
        }
        auto base_ins = base_op->get_inputs();
        auto base_out = base_op->get_outputs();
        base_op_out_size += base_out.size();
        for (size_t i = 0; i < base_ins.size(); i++) {
            COMPILE_ASSERT(
                    bw_axes_map.haskey(base_ins[i]), "Unexpected cases found");
            op_traits::batchwise_shrinkable_t::record_shrinked_axes(
                    bw_axes_map, fused_ins[i], bw_axes_map.get(base_ins[i]));
        }
    }
    // collect fusion graph
    collect_shrinked_graph_axes_map(mgr_->get_graph(), bw_axes_map, bw_size);

    // collect fused_op self
    auto fmgr_inop = mgr_->get_graph().get_input_ops();
    auto fmgr_outop = mgr_->get_graph().get_output_ops();
    std::vector<graph_tensor_ptr> fmgr_ins(fmgr_inop.size()),
            fmgr_out(fmgr_outop.size());
    std::transform(fmgr_inop.begin(), fmgr_inop.end(), fmgr_ins.begin(),
            [&](const sc_op_ptr &op) { return op->get_outputs()[0]; });
    std::transform(fmgr_outop.begin(), fmgr_outop.end(), fmgr_out.begin(),
            [&](const sc_op_ptr &op) { return op->get_inputs()[0]; });
    for (size_t i = 0; i < fmgr_ins.size(); i++) {
        COMPILE_ASSERT(
                bw_axes_map.haskey(fmgr_ins[i]), "Unexpected cases found");
        op_traits::batchwise_shrinkable_t::record_shrinked_axes(bw_axes_map,
                fused_ins[i + base_op_out_size], bw_axes_map.get(fmgr_ins[i]));
    }
    for (size_t i = 0; i < fmgr_out.size(); i++) {
        COMPILE_ASSERT(
                bw_axes_map.haskey(fmgr_out[i]), "Unexpected cases found");
        op_traits::batchwise_shrinkable_t::record_shrinked_axes(
                bw_axes_map, fused_out[i], bw_axes_map.get(fmgr_out[i]));
    }
}

bool fused_op_t::compare_contents(const sc_op *other) const {
    if (!sc_op::compare_contents(other)) { return false; }
    if (auto other_fused = other->dyn_cast<const fused_op_t>()) {
        if (main_op_.empty() != other_fused->main_op_.empty()) { return false; }
        if (!main_op_.empty()) {
            auto mainop = dynamic_cast<sc_op *>(get_main_op());
            auto other_mainop
                    = dynamic_cast<sc_op *>(other_fused->get_main_op());
            if (!mainop->compare_contents(other_mainop)) { return false; }
        }
        return compare_graph(mgr_->get_graph(), other_fused->mgr_->get_graph());
    }
    return false;
}

// may need refactor when enable graph hash
size_t fused_op_t::hash_contents() const {
    size_t seed = 0;
    hash_combine(seed, sc_op::hash_contents());
    if (!main_op_.empty()) {
        auto mainop = dynamic_cast<sc_op *>(get_main_op());
        hash_combine(seed, mainop->hash_contents());
    }
    return seed;
}

const dispatch_set_ptr &fused_op_t::get_dispatch_key_set() const {
    assert(info_.dispatch_key_set_);
    return info_.dispatch_key_set_;
}

dispatch_set_ptr &fused_op_t::get_dispatch_key_set() {
    if (!info_.dispatch_key_set_) {
        int dummy_num;
        info_.dispatch_key_set_ = std::make_shared<combined_dispatch_key_set_t>(
                get_inner_dispatch_key_sets(&dummy_num));
    }
    return info_.dispatch_key_set_;
}

std::vector<dispatch_set_ptr> fused_op_t::get_inner_dispatch_key_sets(
        int *total_key_num) {
    std::vector<dispatch_set_ptr> ret;
    if (total_key_num) { *total_key_num = 0; }
    if (!main_op_.empty()) {
        ret.emplace_back(main_op_.ops_[1]->get_dispatch_key_set());
        if (total_key_num) {
            *total_key_num
                    += static_cast<int>(main_op_.ops_[1]->get_inputs().size()
                            + main_op_.ops_[1]->get_outputs().size());
        }
    }
    for (auto &op : mgr_->get_graph().ops_) {
        if (op->isa<reorder_op_t>()) {
            ret.emplace_back(op->get_dispatch_key_set());
            if (total_key_num) {
                *total_key_num += static_cast<int>(
                        op->get_inputs().size() + op->get_outputs().size());
            }
        }
    }
    return ret;
}

ir_module_ptr fused_op_t::get_dynamic_query_func(const context_ptr &ctx) {
    auto modu = std::make_shared<ir_module_t>(ctx);
    mgr_->get_graph().sync_dynamic_info_with_graph(get_owner_graph());
    auto orig_2_fmgr_graph = attrs_.get<
            std::unordered_map<graph_tensor_ptr, graph_tensor_ptr>>(
            "temp.orig_to_inner_ltsrs");
    std::unordered_map<graph_tensor_ptr, graph_tensor_ptr> fmgr_2_orig;
    for (auto &kv : orig_2_fmgr_graph) {
        fmgr_2_orig[kv.second] = kv.first;
    }
    // inner graph logical tensor visit states, for query pruning.
    std::unordered_map<graph_tensor_ptr, bool> visited;
    std::vector<expr> ins, outs, in_fmts, out_fmts, ori_ins, ori_in_fmts,
            out_sizes;
    expr main_table_var, kernel;
    size_t inp_idx = 0, out_idx = 0;
    auto func = graph::create_query_func_decl_for_op(this, ins, ori_ins, outs,
            in_fmts, ori_in_fmts, out_fmts, out_sizes, kernel);
    // inner logical tensor => real tensor, out_size and format.
    std::unordered_map<graph_tensor_ptr, tsr_info_t> ltsr2rtsr;
    // build query function body
    builder::ir_builder_t bld;
    bld.push_scope();
    // create dummy tensor/var for inner query.
    expr dummy_kernel = builder::make_var(datatypes::pointer, "dummy_kernel");
    bld.push_var_tensor_def(dummy_kernel, linkage::local);
    expr dummy_size = builder::make_tensor("dummy_size", {1}, datatypes::index);
    bld.push_var_tensor_def(dummy_size, linkage::local);

    // construct combined tensors for final query.
    int total_key_num = 0;
    int dispatch_op_num = static_cast<int>(
            get_inner_dispatch_key_sets(&total_key_num).size());
    int cur_op_idx = 0, cur_key_idx = 0;
    std::vector<int> each_op_num_keys(dispatch_op_num, 0);
    auto main_table_name
            = op_name_ + "__" + std::to_string(logical_op_id_) + "_ptr_table";
    main_table_var = builder::make_var(datatypes::pointer, main_table_name);
    expr combined_keys = builder::make_tensor(
            "combined_keys", {total_key_num}, datatypes::pointer);
    expr combined_algs = builder::make_tensor(
            "combined_algs", {dispatch_op_num}, datatypes::s32);
    bld.push_var_tensor_def(combined_keys);
    bld.push_var_tensor_def(combined_algs);
    auto inp_op_in_mgr = mgr_->get_graph().get_input_ops();
    int inner_tsr_count = 0;
    auto &inputs = get_inputs();
    auto &outputs = get_outputs();
    for (auto &in : inputs) {
        auto it = orig_2_fmgr_graph.find(in);
        COMPILE_ASSERT(it != orig_2_fmgr_graph.end(),
                "Can not find input/output tensor in fused op inner map.");
        ltsr2rtsr[it->second]
                = tsr_info_t(ins[inp_idx], expr(), in_fmts[inp_idx], expr());
        inp_idx++;
    }
    for (auto &out : outputs) {
        auto it = orig_2_fmgr_graph.find(out);
        COMPILE_ASSERT(it != orig_2_fmgr_graph.end(),
                "Can not find input/output tensor in fused op inner map.");
        ltsr2rtsr[it->second] = tsr_info_t(
                outs[out_idx], expr(), out_fmts[out_idx], out_sizes[out_idx]);
        out_idx++;
    }
    auto get_or_create_tsr_and_fmt = [&](const graph_tensor_ptr &in) {
        auto it = ltsr2rtsr.find(in);
        if (it != ltsr2rtsr.end()) { return it->second; }
        auto rtsr
                = builder::make_tensor("tsr_" + std::to_string(inner_tsr_count),
                        {sizeof(runtime::dynamic_tensor_t)}, datatypes::u8);
        auto shape_tsr = builder::make_tensor(
                "dyn_shape_tsr_" + std::to_string(inner_tsr_count),
                {in->details_.get_plain_dims().size()}, datatypes::index);
        shape_tsr->attr().set(attr_keys::no_dead_write, true);
        shape_tsr->attr().set(attr_keys::no_tensor2var, true);
        std::vector<uint64_t> init_format
                = {uint64_t(in->details_.get_format().to_runtime())};
        bool fmt_init = in->details_.get_format_candidates().size() <= 1;
        auto out_fmt = builder::make_tensor(
                "format_" + std::to_string(inner_tsr_count), {1UL},
                datatypes::index, address_space::automatic,
                fmt_init ? std::make_shared<static_data_t>(init_format)
                         : nullptr);
        bld.push_var_tensor_def(rtsr);
        bld.push_var_tensor_def(shape_tsr);
        bld.push_evaluate(builder::make_write_struct(rtsr, shape_tsr,
                dyn_tsr_struct_t::name, dyn_tsr_struct_t::fields::dim_ptr));
        bld.push_evaluate(builder::make_write_struct(rtsr,
                builder::make_constant(
                        {in->details_.get_plain_dims().size()}, datatypes::s32),
                dyn_tsr_struct_t::name, dyn_tsr_struct_t::fields::ndims));
        uint64_t etype = in->details_.dtype_.is_etype_pointer()
                ? in->details_.dtype_.get_pointer_element().as_etype_int()
                : in->details_.dtype_.as_etype_int();
        bld.push_evaluate(builder::make_write_struct(rtsr,
                builder::make_constant({etype}, datatypes::u32),
                dyn_tsr_struct_t::name, dyn_tsr_struct_t::fields::dtype));
        auto plain_shapes
                = get_owner_graph().dims_to_expr(in->details_.get_plain_dims());
        uint64_t dyn_mask_int = 0;
        for (size_t i = 0; i < plain_shapes.size(); i++) {
            bld.push_assign(
                    builder::make_indexing(shape_tsr, {i}), plain_shapes[i]);
            dyn_mask_int |= ((!plain_shapes[i].isa<constant>()) << i);
        }
        bld.push_evaluate(builder::make_write_struct(rtsr,
                builder::make_constant({dyn_mask_int}, datatypes::u8),
                dyn_tsr_struct_t::name, dyn_tsr_struct_t::fields::dyn_mask));
        if (fmt_init) {
            modu->add_global_var(builder::make_var_tensor_def_unattached(
                    out_fmt, linkage::private_global)
                                         .checked_as<define>());
        } else {
            bld.push_var_tensor_def(out_fmt);
        }
        inner_tsr_count++;
        auto ret = tsr_info_t(rtsr, expr(), out_fmt, dummy_size);
        ltsr2rtsr[in] = ret;
        return ret;
    };
    auto need_inner_query = [&](const sc_op_ptr &node, int &main_idx) {
        auto &inputs = node->get_inputs();
        auto &outputs = node->get_outputs();
        if (!can_op_be_dispatched(node)) { return false; }
        for (size_t i = 0; i < inputs.size(); i++) {
            auto &in = inputs[i];
            // original ltensor is legal and is constant
            if (!(visited[in]
                        || (!fmgr_2_orig[in]->uses_.empty()
                                && fmgr_2_orig[in]
                                                ->producer_owner_->attrs_
                                                .get_or_else("constant",
                                                        const_kind::not_const)
                                        != const_kind::not_const))) {
                return true;
            }
        }
        if (node->isa<binary_elementwise_op_t>()) {
            int bc_idx = node->stc_cast<binary_elementwise_op_t>()
                                 ->get_broadcast_input();
            main_idx = bc_idx == -1 ? 0 : 1 - bc_idx;
        }
        for (size_t i = 0; i < outputs.size(); i++) {
            auto &out = outputs[i];
            for (size_t j = 0; j < out->uses_.size(); j++) {
                if (out->uses_[j].second->isa<output_op>()) { return true; }
            }
        }
        return false;
    };
    auto update_op_visited = [&](const sc_op_ptr &node) {
        for (auto &in : node->get_inputs()) {
            visited[in] = true;
        }
        for (auto &out : node->get_outputs()) {
            visited[out] = true;
        }
    };
    auto add_global_table_var = [&](const std::string &table_name,
                                        const op_dispatch_tables_ptr &table_ptr,
                                        const expr &table_var) {
        modu->add_op_table(std::make_pair(table_name, table_ptr));
        auto table_def = builder::make_var_tensor_def_unattached(
                table_var, linkage::private_global);
        modu->add_global_var(table_def.checked_as<define>());
    };
    if (!main_op_.empty()) {
        auto op = main_op_.ops_[1];
        if (op->isa<ops::matmul_core_op_t>()) {
            auto table_ptr = std::make_shared<op_dispatch_tables_t>();
            expr in0 = ins[0], in1 = ins[1];
            expr in_fmt0 = in_fmts[0], in_fmt1 = in_fmts[1];
            expr ori_in0 = ori_ins[0], ori_in1 = ori_ins[1];
            expr ori_in_fmt0 = ori_in_fmts[0], ori_in_fmt1 = ori_in_fmts[1];
            expr out_rtsr, out_fmt, out_size;
            auto table_name = op_name_ + "__" + std::to_string(logical_op_id_)
                    + "_inner__0_table";
            auto table_var = builder::make_var(datatypes::pointer, table_name);
            add_global_table_var(table_name, table_ptr, table_var);
            assert(inp_op_in_mgr[0]->get_outputs().size() == 1);
            auto rhs = get_or_create_tsr_and_fmt(
                    inp_op_in_mgr[0]->get_outputs()[0]);
            visited[op->get_inputs()[0]] = true;
            visited[op->get_inputs()[0]] = true;
            visited[inp_op_in_mgr[0]->get_outputs()[0]] = true;
            out_rtsr = rhs.tensor_;
            out_fmt = rhs.format_;
            out_size = rhs.size_;
            bld.push_evaluate(builtin::call_matmul_core_query_format(table_var,
                    out_rtsr, in0, in1, ori_in0, ori_in1, out_fmt, in_fmt0,
                    in_fmt1, ori_in_fmt0, ori_in_fmt1, out_size, dummy_kernel,
                    builder::tensor_ptr(combined_algs, {cur_op_idx})));
            initialize_format_table_with_op(op, table_ptr);
            // set combined tensor
            bld.push_assign(
                    builder::make_indexing(combined_keys, {cur_key_idx++}),
                    in_fmt0);
            bld.push_assign(
                    builder::make_indexing(combined_keys, {cur_key_idx++}),
                    in_fmt1);
            bld.push_assign(
                    builder::make_indexing(combined_keys, {cur_key_idx++}),
                    out_fmt);
            each_op_num_keys[cur_op_idx] = 3;
            cur_op_idx++;
        } else {
            COMPILE_ASSERT(false, "Currently dynamic only support matmul op.");
        }
    }

    for (auto &op : mgr_->get_graph().ops_) {
        size_t in_size = op->get_inputs().size();
        size_t out_size = op->get_outputs().size();
        std::vector<tsr_info_t> op_outs(out_size), op_ins(in_size);
        for (size_t i = 0; i < out_size; i++) {
            op_outs[i] = get_or_create_tsr_and_fmt(op->get_outputs()[i]);
        }
        for (size_t i = 0; i < in_size; i++) {
            op_ins[i] = get_or_create_tsr_and_fmt(op->get_inputs()[i]);
        }
        // Can not use can_op_be_dispatched as tsr and format need pass through
        // each op.
        if (op->isa<input_op>() || op->isa<output_op>()
                || op->isa<constant_op_t>()) {
            continue;
        }
        auto table_name = op_name_ + "__" + std::to_string(logical_op_id_)
                + "_inner__" + std::to_string(op->logical_op_id_) + "_table";
        auto table_var = builder::make_var(datatypes::pointer, table_name);
        auto table_ptr = std::make_shared<op_dispatch_tables_t>();

        int main_idx = 0;
        if (op->isa<unary_elementwise_op_impl_t>()) {
            if (need_inner_query(op, main_idx)) {
                add_global_table_var(table_name, table_ptr, table_var);
                initialize_format_table_with_op(op, table_ptr);
                bld.push_evaluate(builtin::call_unary_fusible_op_query_format(
                        table_var, op_outs[0].tensor_, op_ins[0].tensor_,
                        op_outs[0].format_, op_ins[0].format_, op_outs[0].size_,
                        dummy_kernel));
            } else {
                auto &out = op->get_outputs()[0];
                ltsr2rtsr[out] = op_ins[main_idx];
            }
        } else if (op->isa<binary_elementwise_op_impl_t>()) {
            if (need_inner_query(op, main_idx)) {
                add_global_table_var(table_name, table_ptr, table_var);
                initialize_format_table_with_op(op, table_ptr);
                bld.push_evaluate(builtin::call_binary_fusible_op_query_format(
                        table_var, op_outs[0].tensor_, op_ins[0].tensor_,
                        op_ins[1].tensor_, op_outs[0].format_,
                        op_ins[0].format_, op_ins[1].format_, op_outs[0].size_,
                        dummy_kernel));
            } else {
                auto &out = op->get_outputs()[0];
                ltsr2rtsr[out] = op_ins[main_idx];
            }
        } else if (op->isa<reorder_op_t>()) {
            // Currently reorder is the last op of fusion pattern, so always
            // query.
            add_global_table_var(table_name, table_ptr, table_var);
            bld.push_evaluate(builtin::call_reorder_op_query_format(table_var,
                    op_outs[0].tensor_, op_ins[0].tensor_, op_outs[0].format_,
                    op_ins[0].format_, op_outs[0].size_, dummy_kernel,
                    builder::tensor_ptr(combined_algs, {cur_op_idx})));
            // set combined key tensor
            bld.push_assign(
                    builder::make_indexing(combined_keys, {cur_key_idx++}),
                    op_ins[0].format_);
            bld.push_assign(
                    builder::make_indexing(combined_keys, {cur_key_idx++}),
                    op_outs[0].format_);
            each_op_num_keys[cur_op_idx] = 2;
            cur_op_idx++;
        } else if (op->isa<reduce_op_t>()) {
            // always query
            add_global_table_var(table_name, table_ptr, table_var);
            initialize_format_table_with_op(op, table_ptr);
            bld.push_evaluate(builtin::call_reduce_op_query_format(table_var,
                    op_outs[0].tensor_, op_ins[0].tensor_, op_outs[0].format_,
                    op_ins[0].format_, op_outs[0].size_, dummy_kernel));
        } else if (op->isa<tensor_view_op_t>()) {
            // always query
            add_global_table_var(table_name, table_ptr, table_var);
            initialize_format_table_with_op(op, table_ptr);
            bld.push_evaluate(builtin::call_tensor_view_op_query_format(
                    table_var, op_outs[0].tensor_, op_ins[0].tensor_,
                    op_outs[0].format_, op_ins[0].format_, op_outs[0].size_,
                    dummy_kernel));
        } else {
            COMPILE_ASSERT(false,
                    "Currently dynamic fusbile op only support unary/binary.");
        }
        update_op_visited(op);
    }
    // final query the fused op kernel.
    assert(cur_key_idx == total_key_num && cur_op_idx == dispatch_op_num);
    auto main_table_ptr = std::make_shared<op_dispatch_tables_t>();
    add_global_table_var(main_table_name, main_table_ptr, main_table_var);
    expr each_op_num_keys_tsr = builder::make_tensor("each_op_num_keys",
            {dispatch_op_num}, datatypes::s32, address_space::automatic,
            std::make_shared<static_data_t>(each_op_num_keys));
    modu->add_global_var(builder::make_var_tensor_def_unattached(
            each_op_num_keys_tsr, linkage::private_global)
                                 .static_as<define>());
    bld.push_evaluate(builtin::call_fused_op_query_combined(main_table_var,
            combined_keys, combined_algs, each_op_num_keys_tsr, dispatch_op_num,
            kernel));

    bld.push_returns(true);
    auto body = bld.pop_scope();
    func->body_ = std::move(body);
    modu->add_func({func});
    modu->set_entry_func_idx(0);
    return modu;
}

std::vector<int> fused_op_t::get_impl_dispatch_candidates() const {
    if (!main_op_.empty()) {
        return main_op_.ops_[1]->get_impl_dispatch_candidates();
    }
    auto &ops = mgr_->get_graph().ops_;
    for (auto &op : ops) {
        if (op->isa<input_op>() || op->isa<output_op>()
                || op->isa<constant_op_t>()) {
            continue;
        }
        return op->get_impl_dispatch_candidates();
    }
    return {};
}

void fused_op_t::update_internal_graph_format(
        const combined_op_dispatch_key_t &key) {
    int key_idx = 0, inp_idx = 0;
    auto &node_inputs = get_inputs();
    if (!main_op_.empty()) {
        auto &inp = main_op_.ops_[0];
        auto &inputs = inp->get_outputs();
        auto &cur_key = key[key_idx++];
        assert(inputs.size() + 1 == cur_key.in_out_formats_.size());
        // update format for both inner op and fused op
        for (size_t i = 0; i < inputs.size(); i++) {
            inputs[i]->details_.set_format(cur_key.in_out_formats_[i]);
            node_inputs[i]->details_.set_format(cur_key.in_out_formats_[i]);
        }
        inp_idx = inputs.size();
        auto top = main_op_.ops_[1]->dyn_cast<tunable_op_t>();
        assert(top);
        top->set_config_by_key(cur_key);
        top->info_.cur_impl_ = cur_key.impl_;
        // tunable op output
        auto &out_format = cur_key.in_out_formats_[inputs.size()];
        main_op_.ops_[1]->get_outputs()[0]->details_.set_format(out_format);
        mgr_->get_graph().ops_[0]->get_outputs()[0]->details_.set_format(
                out_format);
        // update impl alg
        main_op_.ops_[1]->info_.cur_impl_ = cur_key.impl_;
    }
    for (auto &op : mgr_->get_graph().ops_) {
        // currently we store dispatch key of tunable op and reorder only in
        // fused op.
        if (op->isa<reorder_op_t>()) {
            auto &cur_key = key[key_idx++];
            assert(cur_key.in_out_formats_.size() == 2);
            // update format
            op->get_inputs()[0]->details_.set_format(
                    cur_key.in_out_formats_[0]);
            node_inputs[inp_idx++]->details_.set_format(
                    cur_key.in_out_formats_[0]);
            // update impl alg
            op->info_.cur_impl_ = cur_key.impl_;
        }
    }
    assert(key_idx == static_cast<int>(key.size()));
    auto &inner_graph = mgr_->get_graph();
    inner_graph.attrs_.set("insert_reorder", false);
    inner_graph.attrs_.set("is_output_plain", false);
    layout_propagation(inner_graph);

    // sync fused op's input/output format with inner graph
    size_t node_input_offset = main_op_.empty() ? 0 : 2;
    size_t input_offset = main_op_.empty() ? 0 : 1;
    auto inputs = mgr_->get_graph().get_input_ops();
    assert(node_inputs.size() + input_offset
            == inputs.size() + node_input_offset);
    for (size_t i = 0; i + input_offset < inputs.size(); i++) {
        node_inputs[i + node_input_offset]->details_.set_format(
                inputs[i + input_offset]
                        ->get_outputs()[0]
                        ->details_.get_format());
    }
    // update fused op output format
    auto &node_outputs = get_outputs();
    auto outputs = mgr_->get_graph().get_output_ops();
    assert(outputs.size() == node_outputs.size());
    for (size_t i = 0; i < outputs.size(); i++) {
        node_outputs[i]->details_.set_format(
                outputs[i]->get_inputs()[0]->details_.get_format());
    }
}

ir_module_ptr fused_op_t::get_func(context_ptr ctx) {
    main_op_.sync_dynamic_info_with_graph(get_owner_graph());
    mgr_->get_graph().sync_dynamic_info_with_graph(get_owner_graph());
    std::vector<sc_op_ptr> out_failed;
    auto ret = try_get_func(ctx, false, out_failed);
    mgr_->reset_brgemm_register_infos();
    COMPILE_ASSERT(ret && out_failed.empty(),
            "Fusion failed. Fallback not implemented");
    return ret;
}

ir_module_ptr fused_op_t::try_get_func(const context_ptr &ctx, bool just_check,
        std::vector<sc_op_ptr> &out_failed) {
    auto modu = std::make_shared<ir_module_t>(ctx);
    // if no base Op, do fusion on fusible ops
    if (main_op_.empty()) {
        auto &graph = mgr_->get_graph();
        op_dep_matrix_t dep(graph);
        // find correct base input idx for generator
        size_t inp_idx = 0;
        // collect ops which results to broadcast op
        std::vector<sc_op *> bc_dep_ops;
        for (auto &op : graph.ops_) {
            if (auto be_op = op->dyn_cast<binary_elementwise_op_t>()) {
                int bc_idx = be_op->get_broadcast_input();
                if (bc_idx >= 0) { bc_dep_ops.emplace_back(op.get()); }
            }
        }

        for (size_t i = 0; i < graph.get_input_ops().size(); i++) {
            auto ins = graph.get_input_ops()[i];
            // sucessfully found flag
            bool found = true;
            for (auto &op : bc_dep_ops) {
                if (dep.lookup(ins->logical_op_id_, op->logical_op_id_) == 1) {
                    if (auto be_op = op->dyn_cast<binary_elementwise_op_t>()) {
                        int bc_idx = be_op->get_broadcast_input();
                        // ensued by above check
                        COMPILE_ASSERT(bc_idx >= 0,
                                "Implicit broadcast op is expected.");
                        auto bc_arg = op->get_inputs()[bc_idx]->producer_owner_;
                        auto non_bc_arg
                                = op->get_inputs()[1 - bc_idx]->producer_owner_;
                        if (dep.lookup(ins->logical_op_id_,
                                    non_bc_arg->logical_op_id_)
                                        == 1
                                || ins.get() == non_bc_arg) {
                            continue;
                        }
                        if (dep.lookup(
                                    ins->logical_op_id_, bc_arg->logical_op_id_)
                                        == 1
                                || ins.get() == bc_arg) {
                            found = false;
                            break;
                        }
                    } else {
                        COMPILE_ASSERT(
                                0, "Unexpected Op named: " << op->op_name_);
                    }
                }
            }
            if (found) {
                if (get_dims_product(
                            ins->get_outputs()[0]->details_.get_blocking_dims())
                        > get_dims_product(
                                graph.get_input_ops()[inp_idx]
                                        ->get_outputs()[0]
                                        ->details_.get_blocking_dims())) {
                    inp_idx = i;
                }
            }
        }
        // reset input_idx
        mgr_->put_input_first(
                graph.get_input_ops()[inp_idx]->dyn_cast<input_op>());
        outer_loop_generator_t gen(inp_idx);
        return try_lower_fusion_manager(
                ctx, &gen, this, mgr_.get(), true, just_check, out_failed);
    }
    auto mainop = dynamic_cast<tunable_op_t *>(get_main_op());
    COMPILE_ASSERT(mainop, "Expecting tunable_op");

    COMPILE_ASSERT(mainop->get_outputs().size() == 1,
            "Expecting single output tunable op");
    auto gen_ptr = mainop->create_generator();
    mainop->set_config_if_empty(ctx, gen_ptr.get());

    std::vector<expr> ins;
    // real_outs are the output tensors in the function arguments
    std::vector<expr> real_outs;
    auto func = graph::create_func_decl_for_op(this, ins, real_outs);
    assert(mainop->get_outputs().size() <= real_outs.size());
    assert(mainop->get_outputs().size() == 1);
    assert(keep_outputs_.size() == 1);
    // finds if an output can be computed in-place on an "input" of the fusion
    // graph
    auto inplacemap = mgr_->query_inplace();
    // outs are the output tensors for the original outputs of the main op
    std::vector<expr> outs;
    if (keep_outputs_[0]) {
        outs = {real_outs[0]};
    } else if (!inplacemap.at(0).empty() && inplacemap[0][0] == 0) {
        // a really naive inplace optimization. We currently only allow
        // output of fusion replace output of the original output of tunable
        // Op
        outs = {real_outs[0]};
    } else {
        // no in-place available, define the output args independently
        auto &graph = get_owner_graph();
        outs = graph::tensor_detail_to_ir_tensor(graph,
                "__origouts_" + std::to_string(out_idx++),
                mainop->get_outputs());
        assert(outs.size() == 1);
    }
    auto main_op_input_size = mainop->get_inputs().size();
    COMPILE_ASSERT(get_inputs().size() >= main_op_input_size,
            "Input tsr number of Fused op should >= Input tsr number of "
            "original op");
    // additional inputs arg tensors for fusion
    std::vector<expr> additional_ins;
    std::vector<expr> origin_ins(ins.begin(), ins.begin() + main_op_input_size);
    for (size_t i = 0; i < get_inputs().size() - main_op_input_size; i++) {
        additional_ins.emplace_back(ins[i + main_op_input_size]);
    }

    // =======================
    // Start of building function body
    // =======================
    builder::ir_builder_t bld;
    bld.push_scope();
    // for each original outputs
    for (auto &local_out : outs) {
        // if the output is in final output args, do not need to define the
        // output buffer as local tensor
        bool need_define_local = true;
        for (auto &v : real_outs) {
            if (v.ptr_same(local_out)) {
                need_define_local = false;
                break;
            }
        }
        if (need_define_local) {
            local_out->attr()[tensor_shrinker_attrs::may_shrink] = true;
            bld.push_var_tensor_def(local_out);
        }
    }
    std::vector<for_loop> loops;
    bool status = gen_ptr->generate(ctx, mainop->get_config().data_.get(),
            mgr_.get(), origin_ins, outs, loops);
    assert(status);
    bld.push_returns(true);
    auto body = bld.pop_scope();

    // =======================
    // End of building function body
    // =======================
    fuse_state_t fstate;
    std::vector<expr> fuse_outs;
    if (keep_outputs_[0]) {
        assert(real_outs.size() > 1);
        fuse_outs = std::vector<expr>(real_outs.begin() + 1, real_outs.end());
    } else {
        fuse_outs = real_outs;
    }
    if (!just_check) { mgr_->transform_graph(ctx, true); }
    out_failed = mgr_->prepare_and_check(ctx, fstate);
    if (!out_failed.empty()) {
        mgr_->clear_anchor();
        return nullptr;
    }
    if (just_check) {
        mgr_->clear_anchor();
        return nullptr;
    }
    bool can_in_brg = ctx->flags_.brgemm_backend_ == scflags_t::brgemm_t::dnnl
            && mgr_->can_register_brgemm_fusion(body);
    if (!can_in_brg) { mgr_->break_brgemm_fusion(); }
    mgr_->commit(modu, fstate, fuse_outs, additional_ins);
    // register fusion in brgemm.
    if (can_in_brg) {
        body = mgr_->get_brgemm_fusion_register()
                       .remake_brgemm_intrinsic_by_fusion(body);
    }
    func->body_ = std::move(body);
    gen_ptr->schedule_loops(
            ctx, mainop->get_config().data_.get(), func->body_, loops);
    modu->add_func({func});
    modu->set_entry_func_idx(0);
    return modu;
}

void horizontal_fused_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    throw std::runtime_error("horiaontal_fused_op::get_graph Not implemented");
}

horizontal_fused_op_t::horizontal_fused_op_t(const std::string &name,
        const std::vector<sc_op_ptr> &ops_to_merge,
        const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs)
    : ops_to_merge_(ops_to_merge) {
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    op_name_ = name;
    attrs_ = attrs;
}

static std::vector<graph_tensor_ptr> select_graph_tensor_by_idx(
        const std::vector<graph_tensor_ptr> &ins, const std::vector<int> &idx) {
    std::vector<graph_tensor_ptr> outs;
    outs.reserve(idx.size());
    for (auto &i : idx) {
        outs.push_back(ins[i]);
    }
    return outs;
}

static std::vector<expr> select_expr_by_idx(
        const std::vector<expr> &ins, const std::vector<int> &idx) {
    std::vector<expr> outs;
    outs.reserve(idx.size());
    for (auto &i : idx) {
        outs.push_back(ins[i]);
    }
    return outs;
}

void horizontal_fused_op_t::schedule_loops(const stmt &body) {
    COMPILE_ASSERT(body.isa<stmts>(), "body has only one stmt.");
    scope_flatten(body.checked_as<stmts>(), -1);
    std::vector<stmt> &body_seq = body.checked_as<stmts>()->seq_;
    std::vector<for_loop> loops;
    std::vector<stmt> not_loops;
    stmt return_stmt;
    for (auto &st : body_seq) {
        if (st.isa<for_loop>()) {
            loops.push_back(st.checked_as<for_loop>());
        } else if (!st.isa<returns>()) {
            not_loops.push_back(st);
        } else {
            return_stmt = st;
        }
    }
    std::vector<stmt> new_seq(not_loops.begin(), not_loops.end());
    new_seq.insert(new_seq.end(), loops.begin(), loops.end());
    new_seq.push_back(return_stmt);
    body_seq = std::move(new_seq);
    COMPILE_ASSERT(loops.size() > 1,
            "No need to horizontal fuse as parallel loop number is less than "
            "2.");
    for (size_t i = 1; i < loops.size(); i++) {
        loops[0]->parallel_merge(body, loops[i]);
    }
}

ir_module_ptr horizontal_fused_op_t::get_func(context_ptr ctx) {
    auto modu = std::make_shared<ir_module_t>(ctx);
    std::vector<expr> ins, outs;
    auto func = graph::create_func_decl_for_op(this, ins, outs);
    func_inliner_t inliner;
    builder::ir_builder_t bld;
    bld.push_scope();
    for (auto &op : ops_to_merge_) {
        auto &ins_idx = op->attrs_.get<std::vector<int>>("op_ins_idx");
        auto &outs_idx = op->attrs_.get<std::vector<int>>("op_outs_idx");
        op->info_.inputs_ = select_graph_tensor_by_idx(info_.inputs_, ins_idx);
        op->info_.outputs_
                = select_graph_tensor_by_idx(info_.outputs_, outs_idx);
        auto mod_to_merge = op->get_func(ctx);
        auto &global_vars = mod_to_merge->get_module_vars();
        for (auto &def_v : global_vars) {
            modu->add_global_var(def_v);
        }
        auto f = mod_to_merge->get_entry_func();
        tensor_shrinker_t pass;
        f = std::const_pointer_cast<func_base>(pass(f));
        std::vector<expr> op_in_args = select_expr_by_idx(ins, ins_idx);
        std::vector<expr> op_out_args = select_expr_by_idx(outs, outs_idx);
        op_out_args.insert(
                op_out_args.end(), op_in_args.begin(), op_in_args.end());
        auto callf = make_expr<call_node>(f, op_out_args);
        inliner.inline_at(callf, bld.get_current_scope().body, 0, global_vars);
    }
    bld.push_returns(true);
    func->body_ = bld.pop_scope();
    schedule_loops(func->body_);
    modu->add_func({func});
    modu->set_entry_func_idx(0);
    return modu;
}

batchwise_fused_op_t::batchwise_fused_op_t(const std::string &name,
        const sc_dims &bw_dims, const sc_graph_t &graph,
        const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs)
    : bw_dims_(bw_dims) {
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    bw_graph_ = copy_graph(graph);
    op_name_ = name;
    attrs_ = attrs;
}

mixed_fuse_op_t::mixed_fuse_op_t(const std::string &name,
        const std::shared_ptr<mixed_parti_t> &parti, const sc_graph_t &graph,
        const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs) {
    info_.inputs_ = ins;
    info_.outputs_ = outs;
    parti_ = parti;
    sub_graph_ = copy_graph(graph);
    op_name_ = name;
    attrs_ = attrs;
}

ir_module_ptr mixed_fuse_op_t::get_func(context_ptr ctx) {
    auto func = parti_->func_;
    func->name_ += "_" + std::to_string(logical_op_id_);
    func->decl_->name_ += "_" + std::to_string(logical_op_id_);
    schedule_loops(func->body_);
    auto modu = std::make_shared<ir_module_t>(ctx);
    modu->add_func({func});
    modu->set_entry_func_idx(0);
    return modu;
}

void schedule_loop_body(
        const stmt &body, std::unordered_map<expr, expr> *expr_remap) {
    stmt target_loop = body;
    if (target_loop.isa<stmts>()) {
        auto ss = target_loop.static_as<stmts>();
        COMPILE_ASSERT(ss->seq_.size() == 1 && ss->seq_[0].isa<for_loop>(),
                "for loop node is expected");
        target_loop = ss->seq_[0];
    }
    COMPILE_ASSERT(target_loop.isa<for_loop>(), "for loop node is expected");
    for_loop outer_most_loop = target_loop.checked_as<for_loop>();
    outer_most_loop->kind_ = for_type::PARALLEL;
    assert(outer_most_loop.defined());
    const int run_threads = runtime_config_t::get().get_num_threads();
    for_loop cur_loop = outer_most_loop;
    std::vector<for_loop> loops;
    auto fused_number = 1;
    while (true) {
        fused_number *= (get_expr_as_int(cur_loop->iter_end_)
                - get_expr_as_int(cur_loop->iter_begin_));
        if (fused_number / run_threads > 12
                || (fused_number >= run_threads
                        && (fused_number % run_threads) == 0))
            break;
        auto inner_loop = get_inner_for_loop(cur_loop.get());
        if (inner_loop.defined() && !inner_loop->num_threads_
                && (inner_loop->step_.isa<constant>()
                        && get_expr_as_int(inner_loop->step_) == 1)
                && !inner_loop->attr().get_or_else(
                        "temp.loop_no_fuse", false)) {
            std::unordered_map<expr, expr> cur_remap;
            outer_most_loop->fuse(
                    inner_loop, expr_remap ? &cur_remap : nullptr);
            cur_loop = inner_loop;
            if (expr_remap) {
                for (auto &exp_m : (*expr_remap)) {
                    auto iter = cur_remap.find(exp_m.second);
                    if (iter != cur_remap.end()) {
                        exp_m.second = iter->second;
                        cur_remap.erase(iter);
                    }
                }
                expr_remap->insert(cur_remap.begin(), cur_remap.end());
            }
        } else {
            if (cur_loop->body_->attr().has_key("builder.parent_node")) {
                add_parent_node(cur_loop->body_, outer_most_loop);
            }
            break;
        }
    }
}

void mixed_fuse_op_t::schedule_loops(const stmt &body) {
    if (body.isa<for_loop>())
        schedule_loop_body(body);
    else if (body.isa<stmts>()) {
        for (auto &st : body.checked_as<stmts>()->seq_) {
            if (st.isa<for_loop>()) {
                schedule_loop_body(st);
            } else {
                // recursively call
                schedule_loops(st);
            }
        }
    }
}

void mixed_fuse_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    throw std::runtime_error("mixed_fuse_op_t::get_graph Not implemented");
}

struct inplace_recursion_context_t {
    int depth_ = 0;
    // UNDEF means a tensor is not directly or indirectly connected to an input
    enum kind_t { UNDEF = 0, NO_INPLACE, ZERO_OFFSET_INPLACE, FREE_INPLACE };

    // the graph input tensor -> its index in std::vector<kind_t>
    const std::unordered_map<graph_tensor_ptr, int> &tsr_2_in_index_;
    // the map of all graph tensors -> a vector of graph inputs. Each element of
    // the vector represents the in-place status of a graph input
    std::unordered_map<graph_tensor_ptr, std::vector<kind_t>> result_;

    inplace_recursion_context_t(
            const std::unordered_map<graph_tensor_ptr, int> &tsr_2_in_index)
        : tsr_2_in_index_(tsr_2_in_index) {}

    // merges the in-place results. Used when an Op depends on multiple graph
    // inputs and we need to merge the status of the same input
    static kind_t merge_result(kind_t a, kind_t b, bool good_op) {
        if (good_op) {
            if (a == NO_INPLACE || b == NO_INPLACE) { return NO_INPLACE; }
            if (a == UNDEF) { return b; }
            if (b == UNDEF) { return a; }
            if (a == ZERO_OFFSET_INPLACE || b == ZERO_OFFSET_INPLACE) {
                return ZERO_OFFSET_INPLACE;
            }
            return FREE_INPLACE;
        } else {
            // if the current op is not a "good" op, we need to mark all inputs
            // it depends on with NO_INPLACE
            if (a != UNDEF || b != UNDEF) { return NO_INPLACE; }
            return UNDEF;
        }
    }

    // the main recursion function to recursively find the in-place status
    const std::vector<kind_t> *call(const graph_tensor_ptr &tsr) {
        depth_++;
        if (depth_ > 500) { return nullptr; }
        auto itr = result_.find(tsr);
        if (itr != result_.end()) { return &itr->second; }
        auto &ret
                = (result_[tsr] = std::vector<kind_t>(tsr_2_in_index_.size()));
        // if the tensor is used more than once, we simply skip it for the sake
        // of correctness. We can obviously do better than this.
        auto producer = tsr->producer_owner_;
        if (producer->isa<input_op>()) {
            // we define that input tensor can in-place reuse itself.
            ret.at(tsr_2_in_index_.find(tsr)->second) = FREE_INPLACE;
            depth_--;
            return &ret;
        }
        bool good_op = tsr->uses_.size() <= 1UL;
        bool is_binary = producer->isa<binary_elementwise_op_t>();
        // if it is an broadcast op, we cannot in-place reuse the broadcast
        // input
        int bcast_idx = -1;
        if (is_binary) {
            bcast_idx = producer->stc_cast<binary_elementwise_op_t>()
                                ->get_broadcast_input();
        }
        bool must_zero_offset = producer->isa<cast_op_t>() || is_binary
                || producer->isa<unary_elementwise_op_t>();
        bool can_be_free = producer->isa<tensor_view_op_t>();
        good_op = good_op && (must_zero_offset || can_be_free);
        auto &inputs = producer->get_inputs();
        for (size_t input_idx = 0; input_idx < inputs.size(); input_idx++) {
            auto &intsr = inputs[input_idx];
            auto *sub_result = call(intsr);
            if (!sub_result) { return nullptr; }
            for (size_t i = 0; i < ret.size(); i++) {
                auto result = (*sub_result)[i];
                // if the op's input is broadcast, all the graph input tensors
                // it depends on should be NO_INPLACE
                if ((int64_t)bcast_idx == (int64_t)input_idx
                        && result != UNDEF) {
                    result = NO_INPLACE;
                }
                ret[i] = merge_result(ret[i], result, good_op);
                if (must_zero_offset && ret[i] == FREE_INPLACE) {
                    ret[i] = ZERO_OFFSET_INPLACE;
                }
            }
        }
        depth_--;
        return &ret;
    }
};

std::vector<std::pair<int, std::vector<tensor_inplace_info_t>>>
mixed_fuse_op_t::get_inplace_map() {
    std::vector<std::pair<int, std::vector<tensor_inplace_info_t>>> ret;
    auto in_ops = sub_graph_.get_input_ops();
    // create a map from input tensors to its index
    std::unordered_map<graph_tensor_ptr, int> tsr_2_index;
    for (auto &in : in_ops) {
        for (auto &tsr : in->get_outputs()) {
            auto idx = tsr_2_index.size();
            tsr_2_index[tsr] = idx;
        }
    }
    inplace_recursion_context_t ctx {tsr_2_index};

    // for each output tensors...
    auto out_ops = sub_graph_.get_output_ops();
    size_t out_idx = 0;
    for (auto &out : out_ops) {
        for (auto &outtsr : out->get_inputs()) {
            std::vector<tensor_inplace_info_t> can_inplace;
            auto *rec_ret = ctx.call(outtsr);
            if (!rec_ret) {
                SC_MODULE_WARN << "Max recursion count reached for tensor "
                                  "inplace optimization "
                                  "for fused op";
                return {};
            }
            for (size_t i = 0; i < rec_ret->size(); i++) {
                if ((*rec_ret)[i]
                        == inplace_recursion_context_t::ZERO_OFFSET_INPLACE) {
                    can_inplace.emplace_back(tensor_inplace_info_t {
                            static_cast<int>(i), inplace_kind::ZERO_OFFSET});
                } else if ((*rec_ret)[i]
                        == inplace_recursion_context_t::FREE_INPLACE) {
                    can_inplace.emplace_back(tensor_inplace_info_t {
                            static_cast<int>(i), inplace_kind::FREE});
                }
            }
            if (!can_inplace.empty()) {
                ret.emplace_back(out_idx, std::move(can_inplace));
            }
            out_idx++;
        }
    }
    return ret;
}

ir_module_ptr batchwise_fused_op_t::get_func(context_ptr ctx) {
    gt2gt_map lt_map;
    gt2axes_map axes_map;
    collect_shrinked_graph_lt_map(bw_graph_, lt_map, bw_dims_.size());
    collect_shrinked_graph_axes_map(bw_graph_, axes_map, bw_dims_.size());
    auto sub_graph = shrink_graph(bw_graph_, lt_map);
    // print_graph(sub_graph, std::cout, 1);

    std::vector<sc_op_ptr> sub_args;
    auto orig_inp_ops = bw_graph_.get_input_ops(),
         orig_out_ops = bw_graph_.get_output_ops();
    auto inp_ops = sub_graph.get_input_ops(),
         out_ops = sub_graph.get_output_ops();
    sub_args.insert(sub_args.end(), out_ops.begin(), out_ops.end());
    sub_args.insert(sub_args.end(), inp_ops.begin(), inp_ops.end());
    auto sub_modu = lower_graph(ctx, sub_graph, sub_args);
    auto &sub_vars = sub_modu->get_module_vars();

    for (auto &f : sub_modu->get_contents()) {
        remove_parallel(f);
        if (runtime_config_t::get().trace_mode_
                < runtime_config_t::trace_mode_t::KERNEL) {
            f->attr()[function_attrs::skip_trace] = true;
        }
        f->attr()[function_attrs::no_parallel] = true;
    }
    // std::cout << sub_modu->get_entry_func() << "\n";
    std::vector<expr> ins, outs;
    auto func = graph::create_func_decl_for_op(this, ins, outs);
    auto func_body = builder::make_stmts_unattached({}).checked_as<stmts>();
    func->body_ = func_body;
    auto modu = ir_module_t::from_entry_func(ctx, func);
    modu->merge(*sub_modu);

    std::vector<expr> loop_vars;
    sc_dims loop_ranges(bw_dims_.size());
    for (size_t i = 0; i < loop_ranges.size(); i++) {
        loop_vars.emplace_back(builder::make_var(datatypes::index,
                std::string("__batchwise_iter_") + std::to_string(i)));
    }

    std::unordered_map<expr, expr> strided_in_tsr_map, strided_out_tsr_map;
    std::vector<expr> args(ins.size() + outs.size());
    auto transform_new_args = [&](const expr &tsr, const sc_op_ptr &op,
                                      bool is_output) {
        auto dims = get_expr_to_dims(tsr.checked_as<tensor>()->dims_);
        auto bw_axes = axes_map.get(
                is_output ? op->get_inputs()[0] : op->get_outputs()[0]);
        COMPILE_ASSERT(bw_axes.size() == bw_dims_.size(),
                "batchwise axes size should be equal to bw dims")
        std::vector<expr> offset(dims.size(), 0);
        constant_folder_t f;
        bool strided = false;
        sc_dims shrink_dims = dims;
        for (size_t i = 0; i < bw_dims_.size(); i++) {
            if (bw_axes[i] == -1) continue;
            if (shrink_dims[bw_axes[i]] == 1) {
                offset[bw_axes[i]] = 0;
            } else {
                shrink_dims[bw_axes[i]] /= bw_dims_[i];
                offset[bw_axes[i]]
                        = dim2unsigned(shrink_dims[bw_axes[i]]) * loop_vars[i];
            }
            if ((i > 0 && shrink_dims[bw_axes[i]] != 1)
                    || bw_axes[i] != static_cast<int>(i))
                strided = true;
        }
        if (strided) {
            auto &tsr_map
                    = is_output ? strided_out_tsr_map : strided_in_tsr_map;
            // TODO(xxx): consider making a strided tensor here
            auto shrinked_tsr = builder::make_tensor(std::string("strided_")
                            + (is_output ? std::string("out_")
                                         : std::string("in_"))
                            + std::to_string(tsr_map.size()),
                    sub_graph.dims_to_expr(shrink_dims),
                    tsr.checked_as<tensor>()->elem_dtype_);
            shrinked_tsr->attr().set("temp.bw_axes", bw_axes);
            tsr_map[tsr] = shrinked_tsr;
            return shrinked_tsr;
        } else {
            return builder::tensor_ptr(tsr, offset, {}, true);
        }
    };
    std::transform(outs.begin(), outs.end(), orig_out_ops.begin(), args.begin(),
            [&](const expr &tsr, const sc_op_ptr &out_op) {
                return transform_new_args(tsr, out_op, true);
            });

    std::transform(ins.begin(), ins.end(), orig_inp_ops.begin(),
            args.begin() + outs.size(),
            [&](const expr &tsr, const sc_op_ptr &in_op) {
                return transform_new_args(tsr, in_op, false);
            });

    auto declare_strided_tsr_ir
            = [](stmt &body, std::unordered_map<expr, expr> &strided_tsr_map) {
                  for (auto &m : strided_tsr_map) {
                      auto shrinked_tsr = m.second.checked_as<tensor>();
                      body.checked_as<stmts>()->seq_.emplace_back(
                              builder::make_var_tensor_def_unattached(
                                      shrinked_tsr));
                  }
              };

    auto gen_copy_strided_tsr_ir = [&ctx](stmt &body,
                                           std::unordered_map<expr, expr>
                                                   &strided_tsr_map,
                                           const std::vector<expr> &lpvars,
                                           bool orig2shrink) {
        for (auto &m : strided_tsr_map) {
            auto orig_tsr = m.first.checked_as<tensor>(),
                 shrinked_tsr = m.second.checked_as<tensor>();
            std::vector<expr> loop_idx, shrinked_idx, orig_idx;
            auto orig_dims = get_expr_to_dims(orig_tsr->dims_);
            auto shriked_dims = get_expr_to_dims(shrinked_tsr->dims_);
            COMPILE_ASSERT(shrinked_tsr->attr().has_key("temp.bw_axes"),
                    "bw axes could not be found");
            auto bw_axes = shrinked_tsr->attr().get<std::vector<int>>(
                    "temp.bw_axes");
            shrinked_tsr->attr().remove("temp.bw_axes");
            int step = static_cast<int>(ctx->get_max_vector_lanes(
                    shrinked_tsr->elem_dtype_.type_code_));
            bool vectorized = ((shriked_dims.back() % step == 0)
                                      && (shriked_dims.back() >= step))
                    && ((orig_dims.back() % step == 0)
                            && (orig_dims.back() >= step));
            for (size_t i = 0; i < shriked_dims.size(); i++) {
                loop_idx.emplace_back(builder::make_var(datatypes::index,
                        std::string("_strided_cpy_iter") + std::to_string(i)));
                shrinked_idx.emplace_back(loop_idx.back());
                auto iter = std::find(
                        bw_axes.begin(), bw_axes.end(), static_cast<int>(i));
                if (iter != bw_axes.end())
                    orig_idx.emplace_back(lpvars[iter - bw_axes.begin()]
                                    * dim2unsigned(shriked_dims[i])
                            + loop_idx.back());
                else
                    orig_idx.emplace_back(loop_idx.back());
            }
            auto shrink_indexing = builder::make_indexing(
                    shrinked_tsr, shrinked_idx, vectorized ? step : 1);
            auto orig_indexing = builder::make_indexing(
                    orig_tsr, orig_idx, vectorized ? step : 1);
            auto cur = builder::make_stmts_unattached(
                    {builder::make_assign_unattached(
                            orig2shrink ? shrink_indexing : orig_indexing,
                            orig2shrink ? orig_indexing : shrink_indexing)});
            for (int i = shriked_dims.size() - 1; i >= 0; i--) {
                cur = builder::make_for_loop_unattached(loop_idx[i], 0,
                        dim2unsigned(shriked_dims[i]),
                        (i == static_cast<int>(shriked_dims.size() - 1))
                                        && vectorized
                                ? step
                                : 1,
                        make_stmt<stmts_node_t>(std::vector<stmt> {cur}), true,
                        for_type::NORMAL);
            }
            body.checked_as<stmts>()->seq_.emplace_back(cur);
        }
    };

    func_inliner_t inliner;
    stmt cur = builder::make_stmts_unattached({});
    declare_strided_tsr_ir(cur, strided_in_tsr_map);
    declare_strided_tsr_ir(cur, strided_out_tsr_map);
    gen_copy_strided_tsr_ir(cur, strided_in_tsr_map, loop_vars, true);

    auto the_call = builder::make_call(sub_modu->get_entry_func(), args)
                            .checked_as<call>();
    inliner.inline_at(the_call, cur.checked_as<stmts>()->seq_,
            cur.checked_as<stmts>()->seq_.size(), sub_vars);
    gen_copy_strided_tsr_ir(cur, strided_out_tsr_map, loop_vars, false);
    // std::cout << cur << "\n";
    std::reverse_copy(bw_dims_.begin(), bw_dims_.end(), loop_ranges.begin());
    std::reverse(loop_vars.begin(), loop_vars.end());
    for (size_t i = 0; i < loop_ranges.size(); i++) {
        cur = builder::make_for_loop_unattached(loop_vars[i], 0,
                dim2unsigned(loop_ranges[i]), 1,
                make_stmt<stmts_node_t>(std::vector<stmt> {cur}), true,
                for_type::NORMAL);
    }
    schedule_loops(cur);
    func_body->seq_.emplace_back(cur);
    auto ret = builder::make_returns_unattached(true);
    func_body->seq_.emplace_back(ret);
    // std::cout << func << "\n";

    return modu;
}

void batchwise_fused_op_t::schedule_loops(const stmt &body) {
    schedule_loop_body(body);
}

void batchwise_fused_op_t::get_graph_impl(std::shared_ptr<sc_graph_t> &graph) {
    throw std::runtime_error("batchwise_fused_op_t::get_graph Not implemented");
}

} // namespace sc
