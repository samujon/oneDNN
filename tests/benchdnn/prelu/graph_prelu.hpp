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

#ifndef GRAPH_PRELU_HPP
#define GRAPH_PRELU_HPP

#include "dnnl_common.hpp"
#include "dnnl_graph_common.hpp"
#include "prelu/prelu.hpp"

namespace benchdnnext {
namespace prelu {

struct prelu_graph_prb_t : public graph_prb_t {
    prelu_graph_prb_t(const ::prelu::prb_t *prb) : spec_(prb) {
        const auto stop_work = [](const fill_status_t s) {
            return s != fill_status::DONE
                    && s != fill_status::UNHANDLED_CONFIG_OPTIONS;
        };

        ctor_status = handle_main_op_();
        if (stop_work(ctor_status)) return;

        ctor_status = fill_status::DONE;
    };
    fill_status_t ctor_status;

private:
    struct spec_t {
        spec_t(const ::prelu::prb_t *prb) noexcept;
        dims_t data_dims, slope_dims;
        dt sdt;
        std::string data_format {"NXC"}, data_tag;
        bool broadcast_to_channel {false};
    };

    spec_t spec_;

    fill_status_t handle_main_op_();

    dnnl::graph::op::kind get_main_op_kind() const noexcept override {
        return dnnl::graph::op::kind::PReLU;
    }

public:
    const spec_t &spec() const noexcept { return spec_; }
};
int doit(const ::prelu::prb_t *prb, res_t *res);

} // namespace prelu
} // namespace benchdnnext
#endif
