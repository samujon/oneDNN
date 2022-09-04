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

#include <algorithm>
#include <deque>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include "interface/op_schema.hpp"
#include "utils/pm/nested_matcher.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace utils {
namespace pm {

namespace {
// check if an op's inputs are commutative
bool has_commutative_inputs(op_t *op) {
    const op_schema_t *opm
            = op_schema_registry_t::get_op_schema(op->get_kind());
    return opm->get_commutative_inputs();
}

// check if a pb_node is optional and its producers are all optional
bool check_optional_inputs(pb_node_t *n) {
    if (n->get_node_kind() != pb_node_kind::PB_NODE_KIND_REPETITION)
        return false;
    repetition_t *rep_node = dynamic_cast<repetition_t *>(n);
    if (rep_node->get_min_rep() != 0) return false;

    std::vector<std::pair<iport_t, producer_t>> node_inputs = n->get_inputs();
    if (node_inputs.empty()) return true;
    /* for optional input, only 1 producer is supported
           opt_input?
              |
             n?
     */
    if (node_inputs.size() != 1) return false;
    return check_optional_inputs(node_inputs[0].second.first);
}

// fill local context in map when optional exists
void fill_optional_in_map(match_context_t *local_ctx, pb_node_t *cur_node,
        op_t *cur_op, size_t cur_op_port = 0) {
    fill_local_in_map(local_ctx, cur_node, cur_op, cur_op_port);

    std::vector<std::pair<iport_t, producer_t>> node_inputs
            = cur_node->get_inputs();
    if (node_inputs.empty()) return;

    pb_node_t *next_node = node_inputs[0].second.first;
    fill_optional_in_map(local_ctx, next_node, cur_op);
}

// check if a pb_node's all consumers are optional
bool check_optional_outputs(pb_node_t *n) {
    std::vector<std::pair<oport_t, consumers_t>> node_outputs
            = n->get_outputs();
    if (node_outputs.empty()) return true;
    /* for optional output, only 1 consumer is supported
             n
             |
         opt_output?
     */
    if (node_outputs.size() != 1) return false;
    if (node_outputs[0].second.size() != 1) return false;
    pb_node_t *node_output = node_outputs[0].second[0]->first;
    if (node_output->get_node_kind() != pb_node_kind::PB_NODE_KIND_REPETITION)
        return false;
    repetition_t *rep_node = dynamic_cast<repetition_t *>(node_output);
    if (rep_node->get_min_rep() != 0) return false;

    return check_optional_outputs(node_output);
}

// fill local context out map when optional exists
void fill_optional_out_map(match_context_t *local_ctx, pb_node_t *cur_node,
        op_t *cur_op, size_t cur_op_port = 0) {
    fill_local_out_map(local_ctx, cur_node, cur_op, cur_op_port);

    std::vector<std::pair<oport_t, consumers_t>> node_outputs
            = cur_node->get_outputs();
    if (node_outputs.empty()) return;

    pb_node_t *next_node = node_outputs[0].second[0]->first;
    fill_optional_out_map(local_ctx, next_node, cur_op);
}
} // namespace

binding_t::binding_t(node_bind_kind p_kind, op_t *p_op, size_t p_op_port,
        pb_node_t *p_node, size_t p_port)
    : bind_op {p_op}
    , bind_node {p_node}
    , bind_kind {p_kind}
    , bind_port {p_port}
    , bind_op_port {p_op_port} {}

match_context_t::match_context_t(match_context_t *p_ctx, pb_node_t *p_graph)
    : parent_ctx {p_ctx} {
    graph_ = dynamic_cast<pb_graph_t *>(p_graph);
}

bool match_node_attributes(op_t *op, pb_node_t *node) {
    size_t n_func = node->get_num_decision_functions();
    for (size_t i = 0; i < n_func; i++) {
        if (!(node->get_decision_function(i)(op))) { return false; }
    }
    return true;
}

bool match_node_inputs(op_t *op, pb_node_t *node, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    std::vector<std::pair<iport_t, producer_t>> node_inputs
            = node->get_inputs();
    if (node_inputs.empty()) return true;

    std::unordered_map<op_t *, pb_op_t *> copied_op_map = matched_op_map;

    // a lambda function to match each of the inputs in op and node
    auto match_input
            = [&](size_t op_input_offset, size_t node_input_offset) -> bool {
        pb_node_t *in_node = node_inputs[node_input_offset].second.first;
        std::shared_ptr<value_t> op_in_value = nullptr;
        if (op->num_inputs() > op_input_offset) {
            op_in_value = op->get_input_value(op_input_offset);
        }

        if (!op_in_value || !op_in_value->has_producer()) {
            // pattern node has producer while graph op
            // doesn't have. In this case, only optional
            // can survive
            bool support_optional = check_optional_inputs(in_node);
            if (support_optional) {
                fill_optional_in_map(ctx, in_node, op, op_input_offset);
            }
            return support_optional;
        } else {
            op_t *in_op = op->get_input_op(op_input_offset);
            size_t in_op_oport = op_in_value->get_offset();
            oport_t in_node_oport
                    = node_inputs[node_input_offset].second.second;
            binding_t in_bind(
                    BIND_OUT, in_op, in_op_oport, in_node, in_node_oport);
            // handle the potential zero trip match case, fill context's i/o
            // map with hint op
            in_bind.hint_op = op;
            in_bind.hint_op_port = op_input_offset;
            return match_graph_helper(in_bind, ctx, copied_op_map);
        }
    };

    if (node->get_inputs().size() == VARIADIC_INPUT_NUM) {
        assertm(op->num_inputs() < VARIADIC_INPUT_NUM,
                "variadic input num should be larger than actual op's num of "
                "inputs");
        for (size_t i = 0; i < node_inputs.size(); ++i) {
            iport_t node_iport = node_inputs[i].first;
            if (op->num_inputs() < node_iport + 1) break;
            if (!match_input(node_iport, i)) return false;
        }
    } else if (!has_commutative_inputs(op)) {
        for (size_t i = 0; i < node_inputs.size(); ++i) {
            iport_t node_iport = node_inputs[i].first;
            if (!match_input(node_iport, i)) return false;
        }
    } else { // commutative ops need to consider switching inputs
        size_t matched_op_input_offset = op->num_inputs(); // init with illegal
        for (size_t node_input_offset = 0;
                node_input_offset < node_inputs.size(); ++node_input_offset) {
            for (size_t op_input_offset = 0; op_input_offset < op->num_inputs();
                    ++op_input_offset) {
                if (op_input_offset == matched_op_input_offset) {
                    matched_op_input_offset = op->num_inputs();
                    continue;
                }
                if (match_input(op_input_offset, node_input_offset)) {
                    matched_op_input_offset = op_input_offset;
                    break;
                }
            }
            if (matched_op_input_offset == op->num_inputs()) return false;
        }
    }

    matched_op_map = copied_op_map;
    return true;
}

bool match_node_outputs(op_t *op, pb_node_t *node, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    std::vector<std::pair<oport_t, consumers_t>> node_outputs
            = node->get_outputs();
    if (node_outputs.empty()) return true;
    if (op->num_outputs() < node_outputs.size()) return false;

    // the worst situation for matching node output is that pattern node
    // output and graph op output cannot be matched, in this case,
    // only optional can survive.
    bool support_optional = check_optional_outputs(node);

    std::unordered_map<op_t *, pb_op_t *> copied_op_map = matched_op_map;

    //match output for node and op
    for (auto &node_output : node_outputs) {
        size_t node_oport = node_output.first;
        std::shared_ptr<value_t> op_out_value
                = op->get_output_value(node_oport);
        std::unordered_set<size_t> node_oport_matched_cons;
        // match the op consumers one by one
        for (size_t j = 0; j < op_out_value->get_consumers().size(); j++) {
            auto op_consumer = op_out_value->get_consumers()[j];
            op_t *out_op = &(op_consumer.get_op());
            bool consumer_matched = false;

            for (size_t k = 0; k < node_output.second.size(); k++) {
                auto node_consumer = node_output.second[k];
                pb_node_t *out_node = node_consumer->first;
                // check if the out_node has been matched by previous out_ops
                if (node_oport_matched_cons.count(k)) continue;
                binding_t out_bind(BIND_IN, out_op, op_consumer.get_offset(),
                        out_node, node_consumer->second);
                // handle the potential zero trip match case, fill context's i/o
                // map with hint op
                out_bind.hint_op = op;
                out_bind.hint_op_port = node_oport;
                if (!match_graph_helper(out_bind, ctx, copied_op_map)) {
                    continue;
                } else {
                    consumer_matched = true;
                    node_oport_matched_cons.insert(k);
                    break;
                }
            }

            if (!consumer_matched) {
                // TODO(Yixin): temporary fix sigmoid + multiply = swish
                // After successfully matching sigmoid, multiply is also
                // matched because multiply is sigmoid's out_op.
                // When matching multiply, check if it is already in the
                // matched op_map, if yes, the match is OK
                if (node_output.second.size() == 1
                        && node_output.second[0]->first->get_node_kind()
                                != pb_node_kind::PB_NODE_KIND_OP
                        && copied_op_map.count(out_op))
                    continue;
                //if it's the allow_external_output case, then it's fine
                pb_op_t *p_op = copied_op_map[op];
                const std::unordered_set<oport_t> &external_outputs
                        = p_op->get_allowed_external_outputs();
                if (!external_outputs.empty()
                        && external_outputs.find(node_oport)
                                != external_outputs.end()) {
                    continue;
                }
                // if it's the optional case, also fine
                if (support_optional) {
                    fill_optional_out_map(ctx, node, op);
                    return true;
                }
                return false;
            }
        }
        // check if not all consumers of node output are matched
        if (node_oport_matched_cons.size() != node_output.second.size()) {
            if (support_optional) {
                fill_optional_out_map(ctx, node, op);
                return true;
            }
            return false;
        }
    }

    matched_op_map = copied_op_map;
    return true;
}

bool check_cyclic(
        op_t *op, const std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    // internal_ops: ops that are in the matched graph
    std::unordered_set<op_t *> internal_ops;
    for (const auto &kv : matched_op_map)
        internal_ops.insert(kv.first);

    for (size_t op_input_offset = 0; op_input_offset < op->num_inputs();
            ++op_input_offset) {
        std::shared_ptr<value_t> op_in_value
                = op->get_input_value(op_input_offset);
        if (op_in_value->has_producer()) {
            op_t *in_op = op->get_input_op(op_input_offset);
            if (internal_ops.find(in_op) == internal_ops.end()) {
                // in_op is an external_op.
                // recursively traverse the producers of in_op
                // check if any producer is among internal_ops
                // if yes, a cycle exists.
                status_t ret = topo_order_visit({in_op}, [&](op_t *temp_op) {
                    if (internal_ops.find(temp_op) != internal_ops.end())
                        return status::invalid_graph;
                    return status::success;
                });
                // there's a cycle
                if (ret != status::success) return true;
            }
        }
    }

    // no cycle
    return false;
}

bool match_node(const binding_t &b, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    if (b.bind_op == nullptr) return false;
    if (b.bind_node == nullptr) return false;
    if (b.bind_op->get_partition() != nullptr) return false;
    if (b.bind_op->has_attr(op_attr::matched)) return false;
    if (!has_commutative_inputs(b.bind_op) && b.bind_op_port != b.bind_port)
        return false;

    if (!match_node_attributes(b.bind_op, b.bind_node)) return false;

    if (!match_node_inputs(b.bind_op, b.bind_node, ctx, matched_op_map))
        return false;

    if (check_cyclic(b.bind_op, matched_op_map)) return false;

    if (!match_node_outputs(b.bind_op, b.bind_node, ctx, matched_op_map))
        return false;

    return true;
}

bool resolve_node(const binding_t &bind_arg, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    bool success = false;
    switch (bind_arg.bind_node->get_node_kind()) {
        case pb_node_kind::PB_NODE_KIND_ALTERNATION:
            success = match_alternation(bind_arg, ctx, matched_op_map);
            break;
        case pb_node_kind::PB_NODE_KIND_REPETITION:
            success = match_repetition(bind_arg, ctx, matched_op_map);
            break;
        default: break;
    }
    return success;
}

bool match_pattern(op_t *first_op, const std::shared_ptr<pb_graph_t> &pattern,
        std::vector<op_t *> &fusion_ops) {
    match_context_t global_ctx {nullptr, nullptr};
    match_context_t init_ctx {&global_ctx, pattern.get()};
    binding_t init_bind {BIND_NONE, first_op, 0, pattern.get(), 0};
    std::unordered_map<op_t *, pb_op_t *> matched_op_map;
    if (!match_graph(init_bind, &init_ctx, matched_op_map)) { return false; }

    fusion_ops = reorder_matched_list(matched_op_map);

    return true;
}

inline std::vector<op_t *> reorder_matched_list(
        const std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    // split ops and pb_op_ts
    std::vector<op_t *> fusion_ops;
    std::vector<pb_op_t *> pb_op_ts;
    for (auto kv : matched_op_map) {
        fusion_ops.push_back(kv.first);
        pb_op_ts.push_back(kv.second);
    }

    // a deque of op indexes in fusion_ops
    std::deque<op_t *> dq;
    // find an end_op to start reorder, find an op whose output isn't in
    // fusion_ops
    for (auto &aop : fusion_ops) {
        bool is_end = true;
        for (auto &output : aop->get_output_values()) {
            for (auto &consumer : output->get_consumers()) {
                if (std::find(fusion_ops.begin(), fusion_ops.end(),
                            &(consumer.get_op()))
                        != fusion_ops.end()) {
                    is_end = false;
                    break;
                }
            }
            if (!is_end) { break; }
        }
        if (is_end) {
            dq.push_back(aop);
            break;
        }
    }
    std::unordered_set<op_t *> visited;
    std::vector<op_t *> reordered_fusion_ops;
    // sort the fusion_ops to topo order
    while (!dq.empty()) {
        op_t *op = dq.front();
        if (visited.find(op) != visited.end()) {
            dq.pop_front();
            continue;
        }
        bool ready = true;
        auto &inputs = op->get_input_values();
        for (auto it = inputs.rbegin(); it != inputs.rend(); ++it) {
            if ((*it)->has_producer()) {
                op_t &producer = (*it)->get_producer();
                // if current op's producers have not been visited
                // current op is not ready, need to visit its
                // producers first
                if (visited.find(&producer) == visited.end()
                        && std::find(fusion_ops.begin(), fusion_ops.end(),
                                   &producer)
                                != fusion_ops.end()) {
                    dq.push_front(&producer);
                    ready = false;
                }
            }
        }
        auto &outputs = op->get_output_values();
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            auto cons = (*it)->get_consumers();
            for (auto &con : (*it)->get_consumers()) {
                op_t &con_op = con.get_op();
                if (std::find(dq.begin(), dq.end(), &con_op) == dq.end()
                        && std::find(fusion_ops.begin(), fusion_ops.end(),
                                   &con_op)
                                != fusion_ops.end()) {
                    dq.push_back(&con_op);
                }
            }
        }
        // all producers of current op have been visited,
        // then current op is ready
        if (ready) {
            // need to check if the corresponding pb_op_t is
            // wildcard before adding it to reordered_fusion_ops
            auto iter = std::find(fusion_ops.begin(), fusion_ops.end(), op);
            size_t index = iter - fusion_ops.begin();
            pb_op_t *corresponding_pb_op_t = pb_op_ts[index];
            // create a temp_op to match, only wildcard can match wildcard
            op_t temp_op {op_kind::Wildcard};
            if (fusion_ops.size() == 1 // single op partition
                    || !match_node_attributes(
                            &temp_op, corresponding_pb_op_t)) {
                // pb_op_t is not a wildcard
                op->set_attr<bool>(op_attr::matched, true);
                reordered_fusion_ops.emplace_back(op);
            }
            visited.insert(op);
        }
    }
    return reordered_fusion_ops;
}

void fill_parent_io_map(
        match_context_t *local_ctx, const binding_t &local_bind) {
    auto parent_ctx = local_ctx->get_parent_context();

    for (const auto &in_port_con : local_ctx->in_port_map) {
        fill_local_in_map(parent_ctx, local_bind.bind_node,
                in_port_con.second.first, in_port_con.second.second);
    }

    for (const auto &out_port_pro : local_ctx->out_port_map) {
        fill_local_out_map(parent_ctx, local_bind.bind_node,
                out_port_pro.second.first, out_port_pro.second.second);
    }
}

void fill_local_in_map(match_context_t *local_ctx, pb_node_t *cur_node,
        op_t *cur_op, size_t cur_op_port) {
    pb_graph_t *graph = local_ctx->get_graph();
    if (!graph) return;

    // TODO(Zitian): restrict to 1 port 1 consumer in pattern def
    auto inner_cons = graph->get_inner_consumers();
    if (inner_cons.empty()) return;

    for (size_t i = 0; i < inner_cons.size(); ++i) {
        for (size_t j = 0; j < inner_cons[i].second.size(); ++j) {
            size_t iport = inner_cons[i].first;
            const std::shared_ptr<consumer_t> &con = inner_cons[i].second[j];
            if (con->first == cur_node)
                local_ctx->in_port_map[iport] = {cur_op, cur_op_port};
        }
    }
}

void fill_local_out_map(match_context_t *local_ctx, pb_node_t *cur_node,
        op_t *cur_op, size_t cur_op_port) {
    pb_graph_t *graph = local_ctx->get_graph();
    if (!graph) return;

    auto inner_pros = graph->get_inner_producers();
    if (inner_pros.empty()) return;

    assertm(inner_pros.size() == 1, "only support 1 port 1 consumer");
    size_t oport = inner_pros[0].first;
    const producer_t &pro = inner_pros[0].second;
    if (pro.first == cur_node)
        local_ctx->out_port_map[oport] = {cur_op, cur_op_port};
}

bool match_graph_helper(const binding_t &local_bind, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    if (local_bind.bind_node->get_node_kind()
            != pb_node_kind::PB_NODE_KIND_OP) {
        if (matched_op_map.count(local_bind.bind_op)) return true;
        if (!resolve_node(local_bind, ctx, matched_op_map)) return false;
    } else {
        pb_op_t *bind_pb_op = dynamic_cast<pb_op_t *>(local_bind.bind_node);
        // if current op has been visited
        if (matched_op_map.count(local_bind.bind_op)) {
            return matched_op_map[local_bind.bind_op] == bind_pb_op;
        }
        // if current op hasn't been visited
        matched_op_map[local_bind.bind_op] = bind_pb_op;
        if (!match_node(local_bind, ctx, matched_op_map)) {
            matched_op_map.erase(local_bind.bind_op);
            return false;
        }
        // match node success, fill local_context's io ports
        fill_local_in_map(ctx, local_bind.bind_node, local_bind.bind_op,
                local_bind.bind_op_port);
        fill_local_out_map(ctx, local_bind.bind_node, local_bind.bind_op,
                local_bind.bind_op_port);
    }
    return true;
}

bool match_graph(const binding_t &bind_arg, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    binding_t local_bind = bind_arg;
    // Get initial internal node to bind
    switch (bind_arg.bind_kind) {
        case BIND_NONE: {
            local_bind.bind_node = ctx->get_graph()->get_nodes().front();
        } break;
        case BIND_IN: {
            auto consumers
                    = ctx->get_graph()->get_inner_consumer(bind_arg.bind_port);
            // TODO(Yixin) Currently support more than 1 consumer for in_ports
            // But will only traverse from the first consumer
            local_bind.bind_node = (*consumers)[0]->first;
            local_bind.bind_port = (*consumers)[0]->second;
        } break;
        case BIND_OUT: {
            std::shared_ptr<producer_t> prod
                    = ctx->get_graph()->get_inner_producer(bind_arg.bind_port);
            local_bind.bind_node = prod->first;
            local_bind.bind_port = prod->second;
        } break;
        default: {
            return false;
        } break;
    }

    return match_graph_helper(local_bind, ctx, matched_op_map);
}

bool match_alternation(const binding_t &bind_arg, match_context_t *ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    alternation_t *alt_nodes
            = dynamic_cast<alternation_t *>(bind_arg.bind_node);
    for (pb_graph_t *alt_node : alt_nodes->get_alternatives()) {
        std::unordered_map<op_t *, pb_op_t *> temp_op_map = matched_op_map;
        binding_t temp_bind = bind_arg;
        temp_bind.bind_node = alt_node;
        match_context_t local_ctx {ctx, temp_bind.bind_node};
        if (match_graph(temp_bind, &local_ctx, temp_op_map)) {
            matched_op_map = temp_op_map;
            fill_parent_io_map(&local_ctx, bind_arg);
            if (bind_arg.bind_kind != BIND_OUT) {
                // alternation is restricted to have only 1 out port
                if (local_ctx.out_port_map.size() != 1) return false;
                op_t *current_op = local_ctx.out_port_map[0].first;
                return match_node_outputs(
                        current_op, bind_arg.bind_node, ctx, matched_op_map);
            } else {
                // alternation is restricted to have only 1 in port
                if (local_ctx.in_port_map.size() != 1) return false;
                op_t *current_op = local_ctx.in_port_map[0].first;
                return match_node_inputs(
                        current_op, bind_arg.bind_node, ctx, matched_op_map);
            }
        }
    }
    return false;
}

bool match_repetition(const binding_t &bind_arg, match_context_t *parent_ctx,
        std::unordered_map<op_t *, pb_op_t *> &matched_op_map) {
    repetition_t *rep_node = dynamic_cast<repetition_t *>(bind_arg.bind_node);
    port_map pmap = rep_node->get_port_map();
    size_t min_rep = rep_node->get_min_rep();
    size_t max_rep = rep_node->get_max_rep() - 1;

    // binding_t for first iteration.
    // all iterations have same body_graph, bind_kind and bind_port
    // but they have different bind_op.
    // First iteration has the same bind_op as the repetition node.
    binding_t temp_bind = bind_arg;
    temp_bind.bind_node = rep_node->get_body();
    std::unordered_map<op_t *, pb_op_t *> temp_op_map = matched_op_map;

    // a merge context to tag on incremental iterations.
    match_context_t speculative_ctx {parent_ctx, temp_bind.bind_node};
    bool forward_match = temp_bind.bind_kind != BIND_OUT;

    // num of repetition blocks matched
    size_t num_rep = 0;
    while (true) {
        match_context_t temp_ctx {speculative_ctx};
        if (!match_graph(temp_bind, &temp_ctx, temp_op_map)) break;
        ++num_rep;

        // connect previous repetition's out_port_map to
        // current repetition's in_port_map
        if (forward_match) {
            if (num_rep == 1) {
                speculative_ctx.in_port_map.insert(temp_ctx.in_port_map.begin(),
                        temp_ctx.in_port_map.end());
            }
            speculative_ctx.out_port_map.clear();
            speculative_ctx.out_port_map.insert(
                    temp_ctx.out_port_map.begin(), temp_ctx.out_port_map.end());

        } else {
            if (num_rep == 1) {
                speculative_ctx.out_port_map.insert(
                        temp_ctx.out_port_map.begin(),
                        temp_ctx.out_port_map.end());
            }
            speculative_ctx.in_port_map.clear();
            speculative_ctx.in_port_map.insert(
                    temp_ctx.in_port_map.begin(), temp_ctx.in_port_map.end());
        }

        if (num_rep == max_rep) break;

        // prepare for the next round of matching
        // Forward matching
        if (forward_match) {
            temp_bind.bind_kind = BIND_IN;
            oport_t oport = pmap.first;
            op_t *current_op = temp_ctx.out_port_map[oport].first;
            if (oport >= current_op->num_outputs()) break;
            auto cons = current_op->get_output_value(oport)->get_consumers();
            if (cons.empty()) break;
            if (cons.size() == 1) {
                op_t *next_op = &(cons[0].get_op());
                temp_bind.bind_op = next_op;
                temp_bind.bind_op_port = cons[0].get_offset();
            } else {
                // More than 1 consumers. In this case, needs to check
                // if the last node of previous match accepts external
                // output. If no, break
                pb_op_t *current_pb_op = temp_op_map[current_op];
                const std::unordered_set<oport_t> &external_outputs
                        = current_pb_op->get_allowed_external_outputs();
                if (external_outputs.empty()
                        || external_outputs.find(oport)
                                == external_outputs.end()) {
                    break;
                }
                // If yes, decide which one of the consumers will be used
                // for next round's match
                iport_t iport = pmap.second;
                // start op for last round's match
                op_t *start_op = temp_ctx.in_port_map[iport].first;
                pb_op_t *start_pb_op = temp_op_map[start_op];
                op_t *next_op = nullptr;
                size_t next_op_iport = 0;
                for (auto &con : cons) {
                    if (match_node_attributes(&con.get_op(), start_pb_op)) {
                        next_op = &(con.get_op());
                        next_op_iport = con.get_offset();
                        break;
                    }
                }
                if (!next_op) break;
                temp_bind.bind_op = next_op;
                temp_bind.bind_op_port = next_op_iport;
            }
        } else { // backward matching
            temp_bind.bind_kind = BIND_OUT;
            iport_t iport = pmap.second;
            op_t *current_op = temp_ctx.in_port_map[iport].first;
            if (iport >= current_op->num_inputs()) break;
            auto in_value = current_op->get_input_value(iport);
            temp_bind.bind_op = &(in_value->get_producer());
            temp_bind.bind_op_port = in_value->get_offset();
        }
    }

    if (num_rep < min_rep) return false;
    if (num_rep == 0 && min_rep == 0) {
        // Zero trip match
        // need to forward binding to neighboring nodes
        if (forward_match) {
            // nothing needs to be matched, success
            if (bind_arg.bind_node->get_outputs().empty()) {
                if (bind_arg.hint_op) {
                    fill_optional_out_map(parent_ctx, bind_arg.bind_node,
                            bind_arg.hint_op, bind_arg.hint_op_port);
                }
                return true;
            }
            assertm(bind_arg.bind_node->get_outputs().size() == 1,
                    "repetition is restricted to have only 1 output");
            assertm(bind_arg.bind_node->get_consumers(0)->size() == 1,
                    "repetition is restricted to have only 1 output with "
                    "only 1 consumer");
            auto cons = bind_arg.bind_node->get_consumers(pmap.first);
            binding_t con_bind = bind_arg;
            con_bind.bind_node = (*cons)[0]->first;
            if (!match_graph_helper(con_bind, parent_ctx, temp_op_map))
                return false;
        } else {
            if (bind_arg.bind_node->get_inputs().empty()) {
                if (bind_arg.hint_op) {
                    fill_optional_in_map(parent_ctx, bind_arg.bind_node,
                            bind_arg.hint_op, bind_arg.hint_op_port);
                }
                return true;
            }
            binding_t b = bind_arg;
            b.bind_node = b.bind_node->get_producer(pmap.second)->first;
            if (!match_graph_helper(b, parent_ctx, temp_op_map)) return false;
        }
    } else { // num_rep > 0
        fill_parent_io_map(&speculative_ctx, bind_arg);
        if (forward_match) {
            assertm(bind_arg.bind_node->get_outputs().size() <= 1,
                    "repetition is restricted to have only 1 output");

            if (bind_arg.bind_node->get_outputs().size() == 1) {
                assertm(bind_arg.bind_node->get_consumers(0)->size() == 1,
                        "repetition is restricted to have only 1 output with "
                        "only 1 consumer");
                op_t *current_op
                        = speculative_ctx.out_port_map[pmap.first].first;
                if (!match_node_outputs(
                            current_op, rep_node, parent_ctx, temp_op_map))
                    return false;
            }

        } else {
            assertm(bind_arg.bind_node->get_outputs().size() <= 1,
                    "repetition is restricted to have only 1 output");
            if (bind_arg.bind_node->get_outputs().size() == 1) {
                op_t *current_op
                        = speculative_ctx.in_port_map[pmap.second].first;
                if (!match_node_inputs(
                            current_op, rep_node, parent_ctx, temp_op_map))
                    return false;
            }
        }
    }

    matched_op_map = temp_op_map;
    return true;
}

} // namespace pm
} // namespace utils
} // namespace impl
} // namespace graph
} // namespace dnnl
