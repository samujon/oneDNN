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

#include "oneapi/dnnl/dnnl_graph.h"

#include "c_types_map.hpp"
#include "op.hpp"
#include "op_schema.hpp"

using namespace dnnl::graph::impl;

/// constructor
dnnl_graph_op::dnnl_graph_op(
        size_t id, op_kind_t kind, std::string name, bool internal)
    : id_ {id}, kind_ {kind}, name_ {std::move(name)}, internal_ {internal} {
    if (name_.empty()) { name_ = kind2str(kind_) + "_" + std::to_string(id_); }
}

status_t DNNL_GRAPH_API dnnl_graph_op_create(op_t **op, uint64_t id,
        op_kind_t kind, const char *const verbose_name) {
    *op = new op_t {id, kind, verbose_name};
    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_destroy(op_t *op) {
    delete op;
    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_add_input(
        op_t *op, const logical_tensor_t *input) {
    op->add_input(*input);
    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_add_output(
        op_t *op, const logical_tensor_t *output) {
    op->add_output(*output);
    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_set_attr_f32(op_t *op,
        dnnl_graph_op_attr_t name, const float *value, size_t value_len) {
    if (utils::any_null(op, value)) return status::invalid_arguments;

    // value_len = 0 means a single float value, while value_len = 1 means a
    // float vector with size = 1.
    if (value_len == 0) {
        op->set_attr(name, *value);
    } else {
        std::vector<float> val(value, value + value_len);
        op->set_attr(name, val);
    }

    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_set_attr_bool(op_t *op,
        dnnl_graph_op_attr_t name, const uint8_t *value, size_t value_len) {
    if (utils::any_null(op, value)) return status::invalid_arguments;
    if (value_len != 0) return status::invalid_arguments;

    op->set_attr(name, *reinterpret_cast<const bool *>(value));

    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_set_attr_s64(op_t *op,
        dnnl_graph_op_attr_t name, const int64_t *value, size_t value_len) {
    if (utils::any_null(op, value)) return status::invalid_arguments;

    // value_len = 0 means a single integer value, while value_len = 1 means a
    // integer vector with size = 1.
    if (value_len == 0) {
        op->set_attr(name, *value);
    } else {
        std::vector<int64_t> val(value, value + value_len);
        op->set_attr(name, val);
    }

    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_set_attr_str(op_t *op,
        dnnl_graph_op_attr_t name, const char *value, size_t value_len) {
    if (utils::any_null(op, value)) return status::invalid_arguments;
    if (value_len == 0) return status::invalid_arguments;

    op->set_attr(name, std::string(value));

    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_get_id(const op_t *op, size_t *id) {
    *id = op->get_id();
    return status::success;
}

status_t DNNL_GRAPH_API dnnl_graph_op_get_kind(
        const op_t *op, op_kind_t *kind) {
    *kind = op->get_kind();
    return status::success;
}
