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

#ifndef GRAPH_ELTWISE_HPP
#define GRAPH_ELTWISE_HPP

#include "dnnl_graph_common.hpp"
#include "eltwise.hpp"

namespace benchdnnext {
namespace eltwise {

struct eltwise_graph_prb_t : public graph_prb_t {
    eltwise_graph_prb_t(const ::eltwise::prb_t *prb) : spec_(prb) {
        const auto stop_work = [](const fill_status_t s) {
            return s != fill_status::DONE
                    && s != fill_status::UNHANDLED_CONFIG_OPTIONS;
        };

        ctor_status = handle_main_op_();
        if (stop_work(ctor_status)) return;

        ctor_status = fill_status::DONE;
    };
    fill_status_t ctor_status;

    struct spec_t {
        spec_t(const ::eltwise::prb_t *prb);

        float alpha, beta;
        dims_t dims;
        dt eltwise_dt;
        dnnl::graph::op::kind op_kind;
    };
    spec_t spec_;
    fill_status_t handle_main_op_();
};

int doit(const ::eltwise::prb_t *prb, res_t *res);
} // namespace eltwise
} // namespace benchdnnext

#endif
