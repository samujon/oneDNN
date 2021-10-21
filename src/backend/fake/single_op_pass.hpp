/*******************************************************************************
 * Copyright 2021 Intel Corporation
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
#ifndef BACKEND_FAKE_SINGLE_OP_PASS_HPP
#define BACKEND_FAKE_SINGLE_OP_PASS_HPP

#include <memory>
#include <string>

#include "backend/fake/transformation_pass.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace fake_impl {
namespace pass {

using pb_graph_t = utils::pm::pb_graph_t;
using FCreateV2FusedOp = impl::pass::FCreateV2FusedOp;
using FCreateV2Pattern = impl::pass::FCreateV2Pattern;

FAKE_BACKEND_REGISTER_PASSES_DEF_BEGIN(single_op_pass)

#define FAKE_BACKEND_SINGLE_OP_TRANSFORM(name, backend, p) \
    FAKE_BACKEND_REGISTER_TRANSFORMATION_PASS(backend, name) \
            .set_priority(p) \
            .set_attr<FCreateV2Pattern>("FCreateV2Pattern", \
                    [](const std::shared_ptr<pb_graph_t> &pgraph) -> void { \
                        pgraph->append_op(op_kind::Wildcard); \
                    });

// register a wildward matched pass
FAKE_BACKEND_SINGLE_OP_TRANSFORM(wildcard_match_pass, fake, 1.f)

#undef FAKE_BACKEND_SINGLE_OP_TRANSFORM

FAKE_BACKEND_REGISTER_PASSES_DEF_END

} // namespace pass
} // namespace fake_impl
} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
