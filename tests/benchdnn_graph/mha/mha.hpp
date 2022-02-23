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

#ifndef MHA_HPP
#define MHA_HPP

#include <math.h>
#include <random>

#include "common.hpp"
#include "tests/test_thread.hpp"

#include "dnn_types.hpp"
#include "dnnl_common.hpp"
#include "dnnl_debug.hpp"
#include "dnnl_graph_common.hpp"
#include "utils/perf_report.hpp"

namespace mha {
using namespace benchdnnext;
using graph_dt = dnnl::graph::logical_tensor::data_type;

enum config_datatypes {
    CFG_F32 = 0,
    CFG_S8,
    CFG_U8,
    CFG_BF16,
    CFG_DT_MAX,
    CFG_DT_UNSUPPORTED = -1
};

typedef struct dt_conf_t {
    graph_dt dt;
    double min, max; /* representative */
    double f_min, f_max; /* fill range */
    int f_base; /* fill base, use 0 */
    double f_sparsity; /* amount of non-zeros, default 0.25 */
    double f_scale; /* fill scale, scaling factor for integer generated data */
    double eps; /* acceptable error */
} _dt_conf_t[CFG_DT_MAX];

enum tensor_desc {
    QINT8_SRC = 0, //v1,v2,v3 - INT8 input
    QSRC, //v1,v2,v3 - input
    QTSRC, //v1 - transpose input
    QDST, //v1, v2, v3 - input to QK matmul
    KINT8_SRC, //v1,v2,v3 - INT8 input
    KSRC, //v1,v2,v3 - input
    KTSRC, //v1 - transpose input
    KT2SRC, //v1 - k2 transpose input
    VINT8_SRC, //v1,v2,v3 - INT8 input
    VSRC, //v1,v2,v3 - input
    VTSRC, //v1 - transpose input
    VDST, //v1, v2, v3 - input to QKV matmul
    KDST, //v1, v2, v3 - input to QK matmul
    QKMATMULDST, //v1, v2, v3 - output QK matmul
    QKDIVSCALE, //v1, v2, v3 - score scale
    QKDIVDST, //v1, v2, v3 - score output
    QKATTN,
    QKADD,
    QKSOFTMAX, //v1,v2,v3 - softmax output
    QKSOFTMAXQUANCAST, //v3 - softmax cast
    QKSOFTMAXQUAN, //v1 (int8), v2(int8), v3
    QKSOFTMAXDEQUAN, //v1 (int8), v2(int8), v3
    QKSOFTMAXDEQUANCAST, //v3 -  cast
    QKVMATMUL, //v1,v2,v3 - QKVmatmul ouput
    QKVTDST, //v1,v2,v3 - QKVmatmul transpose
    QKVDST, //v1,v2,v3 - QKVmatmul output
    QKVCASTDST, //v3 - QKVmatmul cast output
    QKVINT8DST, //v1 (int8), v2(int8), v3- final ouput
    TENSOR_DESC_TYPES_TOTAL,
};

typedef struct tensor_desc_info {
    bool isavailable;
    std::string tensor_desc_name;
    graph_dt dt;
    dims_t dims;
} _tensor_desc_info[TENSOR_DESC_TYPES_TOTAL];

const int int_max_exact = 1 << 24;
const _dt_conf_t cfg = {
        {graph_dt::f32, -int_max_exact, int_max_exact, -128, 128, 0, 1.0,
                1. / 256, 1e-6},
        {graph_dt::s8, INT8_MIN, INT8_MAX, -5, 5, 0, .35, 1, 0.},
        {graph_dt::u8, 0, UINT8_MAX, 0, 8, 0, .35, 1, 0.},
        {graph_dt::bf16, -int_max_exact, int_max_exact, -128, 128, 0, 1.0,
                1. / 256, 0},
};

int dt2cfg(graph_dt dt);

struct settings_t {
    settings_t() = default;

    // ctor to save certain fields from resetting
    settings_t(const char *perf_template) : settings_t() {
        this->perf_template = perf_template;
    }

    const char *pattern = nullptr;
    prb_dims_t prb_dims;
    std::vector<int> heads {16};
    std::vector<dnnl_data_type_t> dt {dnnl_f32};
    std::vector<std::string> tag {tag::abx};

    std::vector<float> def_scale {0.125, 0.25, 0.5, 1, 2, 4, 8};
    std::vector<attr_t::scale_t> quan_oscale {attr_t::scale_t()};
    std::vector<attr_t::scale_t> dequan_oscale {attr_t::scale_t()};
    std::vector<attr_t::zero_points_t> quan_zero_points {
            attr_t::zero_points_t()};
    std::vector<attr_t::zero_points_t> dequan_zero_points {
            attr_t::zero_points_t()};
    attr_t attr = {};
    const char *perf_template_csv
            = "perf,%engine%,%DESC%,"
              "%-time%,%0time%";
    const char *perf_template_def
            = "perf,%engine%,%impl%,%prb%,%-time%,%0time%";
    const char *perf_template = perf_template_def;

    void reset() { *this = settings_t(perf_template); }
};

struct mha_graph_spec_t {
    mha_graph_spec_t(std::string pattern, const dims_t &dims, const int ndims,
            const int &head, const dnnl_data_type_t &dt,
            const attr_t &quan_attr, const attr_t &dequan_attr,
            float quan_scale, float dequan_scale)
        : mha_pattern(pattern)
        , dims(dims)
        , ndims(ndims)
        , head(head)
        , mha_inout_dt(benchdnnext::convert_dt(dt))
        , quan_attr(quan_attr)
        , dequan_attr(dequan_attr) {
        tag = tag::abx; //TODO: pass from command line
        this->quan_attr.oscale.scale = quan_scale;
        this->dequan_attr.oscale.scale = dequan_scale;
        quan_qtype = convert_attr_policy(quan_attr.oscale.policy);
        dequan_qtype = convert_attr_policy(dequan_attr.oscale.policy);
        generate_scales();
        generate_zero_points();
        MHA_int8 = mha_inout_dt == graph_dt::u8 || mha_inout_dt == graph_dt::s8;
        mha_dt = (mha_inout_dt == graph_dt::bf16 || mha_pattern == "v3")
                ? graph_dt::bf16
                : graph_dt::f32;
    }
    ~mha_graph_spec_t() {}

    std::string mha_pattern;
    dims_t dims;
    int ndims;
    int head;
    int query_sz;
    dnnl::graph::logical_tensor::data_type mha_dt, mha_inout_dt,
            mha_quan_dt {graph_dt::f32};
    std::string tag;
    bool MHA_int8 {false};
    attr_t quan_attr, dequan_attr;
    std::string quan_qtype, dequan_qtype;
    //TODO: zps needs to be modified depending on qtype/policy
    std::vector<int64_t> quan_zps, dequan_zps;
    std::vector<float> quan_scales, dequan_scales;
    void generate_scales();
    void generate_zero_points();
};

std::ostream &operator<<(std::ostream &s, const mha_graph_spec_t &spec);

struct perf_report_t : public base_perf_report_t {
    perf_report_t(const mha_graph_spec_t *spec, const char *perf_template)
        : base_perf_report_t(perf_template), spec_(spec) {}
    void dump_desc(std::ostream &s) const override { s << *spec_; }
    void dump_desc_csv(std::ostream &s) const override { dump_desc(s); }

private:
    const mha_graph_spec_t *spec_;
};

struct mha_graph_prb_t : public ::benchdnnext::graph_prb_t {
    mha_graph_prb_t(const mha_graph_spec_t &spec) {
        const auto stop_work = [](const fill_status_t s) {
            return s != fill_status::DONE
                    && s != fill_status::UNHANDLED_CONFIG_OPTIONS;
        };
        ctor_status = build_mha_subgraph(spec);
        if (stop_work(ctor_status)) return;

        ctor_status = fill_status::DONE;
    };
    fill_status_t ctor_status;

    ~mha_graph_prb_t() {}
    void build_tensor_desc(const mha_graph_spec_t &spec);
    fill_status_t build_mha_subgraph(const mha_graph_spec_t &spec);

private:
    void addDequanOp(const mha_graph_spec_t &spec, const std::string src,
            const std::string dst);
    void addStaticReshapeOp(const mha_graph_spec_t &spec, const std::string src,
            const std::string dst, const dims_t shape);
    void addStaticTransposeOp(const mha_graph_spec_t &spec,
            const std::string src, const std::string, dims_t axis);
    void addQuanOp(const mha_graph_spec_t &spec, const std::string src,
            const std::string dst);
    void addTypecastOp(const std::string src, const std::string dst);
    void addReorderOp(const std::string src, const std::string dst);

public:
    dnnl::graph::op::kind get_main_op_kind() const noexcept override {
        return dnnl::graph::op::kind::MatMul;
    }
};

int doit(const mha_graph_spec_t *spec, res_t *res);
int bench(int argc, char **argv);

} // namespace mha

#endif
