/*******************************************************************************
* Copyright 2018-2022 Intel Corporation
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

#include "oneapi/dnnl/dnnl.h"

#include "rnn/rnn_aux.hpp"
#include "utils/compare.hpp"

namespace rnn {

alg_t str2alg(const char *str) {
#define CASE(_alg) \
    if (!strcasecmp(STRINGIFY(_alg), str)) return _alg
    CASE(VANILLA_RNN);
    CASE(VANILLA_LSTM);
    CASE(VANILLA_GRU);
    CASE(LBR_GRU);
    CASE(VANILLA_AUGRU);
    CASE(LBR_AUGRU);
#undef CASE
    assert(!"unknown algorithm");
    return VANILLA_RNN;
}

const char *alg2str(alg_t alg) {
    if (alg == VANILLA_RNN) return "VANILLA_RNN";
    if (alg == VANILLA_LSTM) return "VANILLA_LSTM";
    if (alg == VANILLA_GRU) return "VANILLA_GRU";
    if (alg == LBR_GRU) return "LBR_GRU";
    if (alg == VANILLA_AUGRU) return "VANILLA_AUGRU";
    if (alg == LBR_AUGRU) return "LBR_AUGRU";
    assert(!"unknown algorithm");
    return "unknown algorithm";
}

dnnl_alg_kind_t alg2kind(alg_t alg) {
    if (alg == VANILLA_RNN) return dnnl_vanilla_rnn;
    if (alg == VANILLA_LSTM) return dnnl_vanilla_lstm;
    if (alg == VANILLA_GRU) return dnnl_vanilla_gru;
    if (alg == LBR_GRU) return dnnl_lbr_gru;
    if (alg == VANILLA_AUGRU) return dnnl_vanilla_augru;
    if (alg == LBR_AUGRU) return dnnl_lbr_augru;
    assert(!"unknown algorithm");
    return dnnl_alg_kind_undef;
}

activation_t str2activation(const char *str) {
#define CASE(_act) \
    if (!strcasecmp(STRINGIFY(_act), str)) return _act
    CASE(RELU);
    CASE(LOGISTIC);
    CASE(TANH);
    CASE(UNDEF);
#undef CASE
    assert(!"unknown activation");
    return UNDEF;
}

const char *activation2str(activation_t act) {
    const char *str = "unknown activation";
    switch (act) {
        case RELU: str = "RELU"; break;
        case LOGISTIC: str = "LOGISTIC"; break;
        case TANH: str = "TANH"; break;
        case UNDEF: str = "UNDEF"; break;
        default: assert(!"unknown activation");
    }
    return str;
}

dnnl_alg_kind_t activation2kind(activation_t act) {
    dnnl_alg_kind_t alg_kind = dnnl_alg_kind_undef;
    switch (act) {
        case RELU: alg_kind = dnnl_eltwise_relu; break;
        case LOGISTIC: alg_kind = dnnl_eltwise_logistic; break;
        case TANH: alg_kind = dnnl_eltwise_tanh; break;
        case UNDEF: alg_kind = dnnl_alg_kind_undef; break;
        default: assert(!"unknown activation");
    }
    return alg_kind;
}

dnnl_rnn_direction_t str2direction(const char *str) {
    if (!strcasecmp("left2right", str)) return dnnl_unidirectional_left2right;
    if (!strcasecmp("right2left", str)) return dnnl_unidirectional_right2left;
    if (!strcasecmp("concat", str)) return dnnl_bidirectional_concat;
    if (!strcasecmp("sum", str)) return dnnl_bidirectional_sum;
    assert(!"unknown direction");
    return dnnl_unidirectional_left2right;
}

const char *direction2str(dnnl_rnn_direction_t direction) {
    if (direction == dnnl_unidirectional_left2right) return "left2right";
    if (direction == dnnl_unidirectional_right2left) return "right2left";
    if (direction == dnnl_bidirectional_concat) return "concat";
    if (direction == dnnl_bidirectional_sum) return "sum";
    assert(!"unknown direction");
    return "unknown direction";
}

const char *rnn_data_kind2str(rnn_data_kind_t kind) {
#define CASE(KIND) \
    if (kind == (KIND)) return STRINGIFY(KIND)
    CASE(SRC_LAYER);
    CASE(AUGRU_ATTENTION);
    CASE(SRC_ITER);
    CASE(SRC_ITER_C);
    CASE(WEIGHTS_LAYER);
    CASE(WEIGHTS_ITER);
    CASE(WEIGHTS_PEEPHOLE);
    CASE(WEIGHTS_PROJECTION);
    CASE(BIAS);
    CASE(DST_LAYER);
    CASE(DST_ITER);
    CASE(DST_ITER_C);

    CASE(DIFF_SRC_LAYER);
    CASE(DIFF_AUGRU_ATTENTION);
    CASE(DIFF_SRC_ITER);
    CASE(DIFF_SRC_ITER_C);
    CASE(DIFF_WEIGHTS_LAYER);
    CASE(DIFF_WEIGHTS_ITER);
    CASE(DIFF_WEIGHTS_PEEPHOLE);
    CASE(DIFF_WEIGHTS_PROJECTION);
    CASE(DIFF_BIAS);
    CASE(DIFF_DST_LAYER);
    CASE(DIFF_DST_ITER);
    CASE(DIFF_DST_ITER_C);
#undef CASE

    assert(!"incorrect rnn data kind");
    return "incorrect rnn data kind";
}

int str2desc(desc_t *desc, const char *str) {
    desc_t d {0};

    /* canonical form:
     * lXtXmbXsicXslcXdhcXdicX
     *
     * where: X is number, S - string
     * note: symbol `_` is ignored
     *
     * implicit rules:
     *  - default values:
     *      l = 1, t = 1, mb = 2
     *  - if slc/dhc is undefined => slc/dhc = sic
     *  - if dic is undefined => dic = dhc
     */

    d.n_layer = 1;
    d.n_iter = 1;
    d.mb = 2;

    const char *s = str;
    assert(s);

#define CASE_NN(prb, c) \
    do { \
        if (!strncmp(prb, s, strlen(prb))) { \
            ok = 1; \
            s += strlen(prb); \
            char *end_s; \
            d.c = strtol(s, &end_s, 10); \
            s += (end_s - s); \
            if (d.c < 0) return FAIL; \
        } \
    } while (0)
#define CASE_N(c) CASE_NN(#c, c)
    while (*s) {
        int ok = 0;
        CASE_NN("l", n_layer);
        CASE_NN("t", n_iter);
        CASE_N(mb);
        CASE_N(sic);
        CASE_N(slc);
        CASE_N(dhc);
        CASE_N(dic);
        if (!strncmp("dlc", s, 3)) {
            BENCHDNN_PRINT(0, "%s\n",
                    "WARNING: the RNN descriptor symbol `dlc` is no longer "
                    "supported. Please adjust the RNN descriptor and try "
                    "again. Note: usually it is enough to simply remove `dlc` "
                    "from the descriptor string.");
            return FAIL;
        }
        if (*s == 'n') {
            d.name = s + 1;
            break;
        }
        if (*s == '_') ++s;
        if (!ok) return FAIL;
    }
#undef CASE_NN
#undef CASE_N

    if (d.sic == 0) return FAIL;
    if (d.slc == 0) d.slc = d.sic;
    if (d.dhc == 0) d.dhc = d.sic;
    if (d.dic == 0) d.dic = d.dhc;
    d.wc = MAX2(MAX2(d.sic, d.slc), MAX2(d.dic, d.dhc));

    *desc = d;

    return OK;
}

std::ostream &operator<<(std::ostream &s, const desc_t &d) {
    s << "l" << d.n_layer << "t" << d.n_iter << "mb" << d.mb << "sic" << d.sic
      << "slc" << d.slc << "dhc" << d.dhc << "dic" << d.dic;

    if (!d.name.empty()) s << "n" << d.name;

    return s;
}

std::ostream &operator<<(std::ostream &s, const prb_t &prb) {
    dump_global_params(s);
    settings_t def;

    if (canonical || prb.prop != prop2prop_kind(def.prop[0]))
        s << "--prop=" << prop2str(prb.prop) << " ";
    if (canonical || prb.cfg.str() != def.cfg[0])
        s << "--cfg=" << prb.cfg.str() << " ";
    if (canonical || prb.alg != def.alg[0])
        s << "--alg=" << alg2str(prb.alg) << " ";
    if (canonical || prb.direction != def.direction[0])
        s << "--direction=" << direction2str(prb.direction) << " ";
    if (canonical || prb.activation != def.activation[0])
        s << "--activation=" << activation2str(prb.activation) << " ";
    if (canonical || prb.skip_nonlinear != def.skip_nonlinear[0])
        s << "--skip-nonlinear=" << bool2str(prb.skip_nonlinear) << " ";
    if (canonical || prb.with_peephole != def.with_peephole[0])
        s << "--with-peephole=" << bool2str(prb.with_peephole) << " ";
    if (canonical || prb.with_projection != def.with_projection[0])
        s << "--with-projection=" << bool2str(prb.with_projection) << " ";
    if (canonical || prb.wei_scales_policy != def.scale_policy[0])
        s << "--scaling=" << prb.wei_scales_policy << " ";
    if (canonical || prb.wei_proj_scales_policy != def.scale_proj_policy[0])
        s << "--scaling-proj=" << prb.wei_proj_scales_policy << " ";
    if (canonical || prb.trivial_strides != def.trivial_strides[0])
        s << "--trivial-strides=" << bool2str(prb.trivial_strides) << " ";

    s << prb.attr;
    s << static_cast<const desc_t &>(prb);

    return s;
}

dnnl_status_t init_rnn_fwd_desc(dnnl_rnn_desc_t *rd, const prb_t &prb,
        dnnl_prop_kind_t prop_kind, const dnnl_memory_desc_t *src_layer_d,
        const dnnl_memory_desc_t *src_iter_d,
        const dnnl_memory_desc_t *src_iter_c_d,
        const dnnl_memory_desc_t *attention_d,
        const dnnl_memory_desc_t *weights_layer_d,
        const dnnl_memory_desc_t *weights_iter_d,
        const dnnl_memory_desc_t *weights_peephole_d,
        const dnnl_memory_desc_t *weights_projection_d,
        const dnnl_memory_desc_t *bias_d, const dnnl_memory_desc_t *dst_layer_d,
        const dnnl_memory_desc_t *dst_iter_d,
        const dnnl_memory_desc_t *dst_iter_c_d) {
    dnnl_alg_kind_t kind = alg2kind(prb.alg);
    dnnl_alg_kind_t f = activation2kind(prb.activation);

    dnnl_status_t init_status;
    switch (kind) {
        case dnnl_vanilla_rnn:
            init_status = dnnl_vanilla_rnn_forward_desc_init(rd, prop_kind, f,
                    prb.direction, src_layer_d, src_iter_d, weights_layer_d,
                    weights_iter_d, bias_d, dst_layer_d, dst_iter_d, prb.flags,
                    prb.alpha, prb.beta);
            break;
        case dnnl_vanilla_lstm:
            init_status = dnnl_lstm_forward_desc_init_v3(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, src_iter_c_d,
                    weights_layer_d, weights_iter_d, weights_peephole_d,
                    weights_projection_d, bias_d, dst_layer_d, dst_iter_d,
                    dst_iter_c_d, prb.flags);
            break;
        case dnnl_vanilla_gru:
            init_status = dnnl_gru_forward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, weights_layer_d,
                    weights_iter_d, bias_d, dst_layer_d, dst_iter_d, prb.flags);
            break;
        case dnnl_lbr_gru:
            init_status = dnnl_lbr_gru_forward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, weights_layer_d,
                    weights_iter_d, bias_d, dst_layer_d, dst_iter_d, prb.flags);
            break;
        case dnnl_vanilla_augru:
            init_status = dnnl_augru_forward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, attention_d,
                    weights_layer_d, weights_iter_d, bias_d, dst_layer_d,
                    dst_iter_d, prb.flags);
            break;
        case dnnl_lbr_augru:
            init_status = dnnl_lbr_augru_forward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, attention_d,
                    weights_layer_d, weights_iter_d, bias_d, dst_layer_d,
                    dst_iter_d, prb.flags);
            break;
        default: init_status = dnnl_unimplemented;
    }
    return init_status;
}

dnnl_status_t init_rnn_bwd_desc(dnnl_rnn_desc_t *rd, const prb_t &prb,
        dnnl_prop_kind_t prop_kind, const dnnl_memory_desc_t *src_layer_d,
        const dnnl_memory_desc_t *src_iter_d,
        const dnnl_memory_desc_t *src_iter_c_d,
        const dnnl_memory_desc_t *attention_d,
        const dnnl_memory_desc_t *weights_layer_d,
        const dnnl_memory_desc_t *weights_iter_d,
        const dnnl_memory_desc_t *weights_peephole_d,
        const dnnl_memory_desc_t *weights_projection_d,
        const dnnl_memory_desc_t *bias_d, const dnnl_memory_desc_t *dst_layer_d,
        const dnnl_memory_desc_t *dst_iter_d,
        const dnnl_memory_desc_t *dst_iter_c_d,
        const dnnl_memory_desc_t *diff_src_layer_d,
        const dnnl_memory_desc_t *diff_src_iter_d,
        const dnnl_memory_desc_t *diff_src_iter_c_d,
        const dnnl_memory_desc_t *diff_attention_d,
        const dnnl_memory_desc_t *diff_weights_layer_d,
        const dnnl_memory_desc_t *diff_weights_iter_d,
        const dnnl_memory_desc_t *diff_weights_peephole_d,
        const dnnl_memory_desc_t *diff_weights_projection_d,
        const dnnl_memory_desc_t *diff_bias_d,
        const dnnl_memory_desc_t *diff_dst_layer_d,
        const dnnl_memory_desc_t *diff_dst_iter_d,
        const dnnl_memory_desc_t *diff_dst_iter_c_d) {
    dnnl_alg_kind_t kind = alg2kind(prb.alg);
    dnnl_alg_kind_t f = activation2kind(prb.activation);

    dnnl_status_t init_status;
    switch (kind) {
        case dnnl_vanilla_rnn:
            init_status = dnnl_vanilla_rnn_backward_desc_init(rd, prop_kind, f,
                    prb.direction, src_layer_d, src_iter_d, weights_layer_d,
                    weights_iter_d, bias_d, dst_layer_d, dst_iter_d,
                    diff_src_layer_d, diff_src_iter_d, diff_weights_layer_d,
                    diff_weights_iter_d, diff_bias_d, diff_dst_layer_d,
                    diff_dst_iter_d, prb.flags, prb.alpha, prb.beta);
            break;
        case dnnl_vanilla_lstm:
            init_status = dnnl_lstm_backward_desc_init_v3(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, src_iter_c_d,
                    weights_layer_d, weights_iter_d, weights_peephole_d,
                    weights_projection_d, bias_d, dst_layer_d, dst_iter_d,
                    dst_iter_c_d, diff_src_layer_d, diff_src_iter_d,
                    diff_src_iter_c_d, diff_weights_layer_d,
                    diff_weights_iter_d, diff_weights_peephole_d,
                    diff_weights_projection_d, diff_bias_d, diff_dst_layer_d,
                    diff_dst_iter_d, diff_dst_iter_c_d, prb.flags);
            break;
        case dnnl_vanilla_gru:
            init_status = dnnl_gru_backward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, weights_layer_d,
                    weights_iter_d, bias_d, dst_layer_d, dst_iter_d,
                    diff_src_layer_d, diff_src_iter_d, diff_weights_layer_d,
                    diff_weights_iter_d, diff_bias_d, diff_dst_layer_d,
                    diff_dst_iter_d, prb.flags);
            break;
        case dnnl_lbr_gru:
            init_status = dnnl_lbr_gru_backward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, weights_layer_d,
                    weights_iter_d, bias_d, dst_layer_d, dst_iter_d,
                    diff_src_layer_d, diff_src_iter_d, diff_weights_layer_d,
                    diff_weights_iter_d, diff_bias_d, diff_dst_layer_d,
                    diff_dst_iter_d, prb.flags);
            break;
        case dnnl_vanilla_augru:
            init_status = dnnl_augru_backward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, attention_d,
                    weights_layer_d, weights_iter_d, bias_d, dst_layer_d,
                    dst_iter_d, diff_src_layer_d, diff_src_iter_d,
                    diff_attention_d, diff_weights_layer_d, diff_weights_iter_d,
                    diff_bias_d, diff_dst_layer_d, diff_dst_iter_d, prb.flags);
            break;
        case dnnl_lbr_augru:
            init_status = dnnl_lbr_augru_backward_desc_init(rd, prop_kind,
                    prb.direction, src_layer_d, src_iter_d, attention_d,
                    weights_layer_d, weights_iter_d, bias_d, dst_layer_d,
                    dst_iter_d, diff_src_layer_d, diff_src_iter_d,
                    diff_attention_d, diff_weights_layer_d, diff_weights_iter_d,
                    diff_bias_d, diff_dst_layer_d, diff_dst_iter_d, prb.flags);
            break;
        default: init_status = dnnl_unimplemented;
    }
    return init_status;
}

void init_buffer(float *buf, int64_t size, float value) {
    for (int64_t i = 0; i < size; i++)
        buf[i] = value;
}

// If needed, dequantize u8 data to f32 via data scale and shift
float maybe_deq(const prb_t &prb, const float in) {
    if (!prb.cfg.is_int8()) return in;
    return (in - prb.data_shift) * (1.0f / prb.data_scale);
}

// If needed, dequantize s32 accumulators to f32 via data and weights scales
// (no data shift is needed as it is handled by the compensation in the bias)
float maybe_deq(const prb_t &prb, const float in, int64_t oc) {
    if (!prb.cfg.is_int8()) return in;
    float scale = prb.get_wei_scale(oc);
    return in * (1.0f / (scale * prb.data_scale));
}

// If needed, dequantize s32 accumulators to f32 via data, weights scales
// and compensation.
float maybe_deq(
        const prb_t &prb, const float in, float scale, float compensation) {
    if (!prb.cfg.is_int8()) return in;
    return (in - compensation * prb.data_shift)
            * (1.0f / (scale * prb.data_scale));
}

float maybe_deq_proj(
        const prb_t &prb, const float in, float compensation, int64_t oc) {
    return maybe_deq(prb, in, prb.get_wei_proj_scale(oc), compensation);
}

float maybe_q(const prb_t &prb, float h) {
    if (!prb.cfg.is_int8()) return h;
    float fp = prb.data_scale * h + prb.data_shift;
    if (fp > prb.cfg[SRC_LAYER].max) fp = prb.cfg[SRC_LAYER].max;
    if (fp < prb.cfg[SRC_LAYER].min) fp = prb.cfg[SRC_LAYER].min;
    fp = mxcsr_cvt(fp);
    return fp;
}

float logistic(float x) {
    if (x < 0)
        return (expf(x) / (1 + expf(x)));
    else
        return 1.0f - (expf(-x) / (1 + expf(-x)));
}
float dlogistic(float x) {
    float tmp = logistic(x);
    return tmp * (1 - tmp);
}
float dtanhf(float x) {
    return (1 - tanhf(x)) * (1 + tanhf(x));
}
float x_m_square(float x) {
    return x - x * x;
}
float relu(float x, float alpha) {
    return x > 0 ? x : alpha * x;
}
float drelu(float x, float alpha) {
    return x > 0 ? 1.0f : alpha;
}
float one_m_square(float x) {
    return 1 - x * x;
}

data_kind_t rnn_data_kind2data_kind(rnn_data_kind_t rnn_data_kind) {
    switch (rnn_data_kind) {
        case DST_LAYER: return DST;
        case DST_ITER: return data_kind_t::DST_ITER;
        case DST_ITER_C: return data_kind_t::DST_ITER_C;
        case DIFF_SRC_LAYER: return SRC;
        case DIFF_AUGRU_ATTENTION: return data_kind_t::AUGRU_ATTENTION;
        case DIFF_SRC_ITER: return data_kind_t::SRC_ITER;
        case DIFF_SRC_ITER_C: return data_kind_t::SRC_ITER_C;
        case DIFF_WEIGHTS_LAYER: return WEI;
        case DIFF_WEIGHTS_ITER: return WEI_ITER;
        case DIFF_WEIGHTS_PEEPHOLE: return WEI_PEEPHOLE;
        case DIFF_WEIGHTS_PROJECTION: return WEI_PROJECTION;
        case DIFF_BIAS: return BIA;
        default: assert(!"unknown data kind");
    }
    return DAT_TOTAL;
}

int compare_dat(const prb_t &prb, rnn_data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, res_t *res) {
    compare::compare_t cmp;
    cmp.set_data_kind(rnn_data_kind2data_kind(kind));

    // factor 2 is because of the sum of 2 GEMMs
    int64_t fwd_acc_dim = 2 * prb.n_gates() + 1;
    if (prb.alg == VANILLA_GRU || prb.alg == VANILLA_AUGRU)
        fwd_acc_dim *= prb.sic;
    int64_t bwdd_acc_dim = prb.n_gates() * prb.dhc;
    int64_t bwdw_acc_dim = prb.mb;
    int64_t acc_dim = fwd_acc_dim;
    if (prb.prop == dnnl_backward) acc_dim *= MAX2(bwdd_acc_dim, bwdw_acc_dim);
    // Here the factor 4 just gives some wiggle room for fp32 testing
    float trh = 4
            * (1 + (prb.prop == dnnl_backward)) // double wiggle room for bwd
            * ((prb.direction == dnnl_bidirectional_sum)
                    + 1) // double trh if bidir_sum
            * ceilf(log2f(acc_dim * prb.n_iter)) * prb.cfg[kind].eps;
    // expect exact value for int8
    if (prb.cfg[kind].dt == dnnl_u8 || prb.cfg[kind].dt == dnnl_s8) trh = 0.f;
    cmp.set_threshold(trh);

    // Note: we do an eltwise comparison only when:
    // - we use skip_nonlinear;
    // - we do not use skip_nonlinear and we test only one cell execution;
    // - for int8 computations the tensor is not DST_ITER_C;
    // If the above conditions are not met, we check only L1, L2 and L8.

    // Rough rationale for the `DST_ITER_C` exception in int8 case:
    // - The formula for one-step c-state is:
    //   c_t = f_t * c_{t−1} + i_t * c~_t.
    //   Here all computations happen in f32 (f_t, i_t, and c~_t are dequantized
    //   right before the computations + the corresponding bias added).
    // - In int8 case we don't have much control over these components and
    //   cannot surmount potential cancellations, if any.
    //   In practice, I observed that the relative element-wise error of values
    //   in `DST_ITER_C` was bigger (up-to 8e-5) whenever the values
    //   themselves were smaller (which indirectly means the problem is exactly
    //   in the cancellation). Unfortunately, this even happened with only one
    //   layer and one time stamp.
    // - So, for now the solution is to use l1- l2- and l_inf-norms to validate
    //   `DST_ITER_C`. When we switch testing on using precise
    //   integer arithmetic based on modulo operation in rnn_tparams (instead of
    //   current unreliable re-scaling), this testing weakness should go away.
    // - Just an obvious side note: `DST_LAYER` and `DST_ITER`
    //   are immediate dequantization of the corresponding u8 tensors. Hence,
    //   as long as we get precise u8 intermediate results (and so far we do),
    //   the f32 result should be pretty accurate -- the dequantization is just
    //   two simple ops: f32 = scale * u8 + shift.
    bool check_norm0
            = (prb.skip_nonlinear || ((prb.n_layer == 1) && (prb.n_iter == 1)));
    if (prb.is_int8() && kind == DST_ITER_C) check_norm0 = false;
    cmp.set_norm_validation_mode(!check_norm0);

    const auto rnn_add_check
            = [&](const compare::compare_t::driver_check_func_args_t &args) {
                  // Limitation from current filling.
                  // TODO: find a better filling to get rid of this...
                  if ((prb.alg == LBR_GRU || prb.alg == LBR_AUGRU
                              || prb.alg == VANILLA_RNN)
                          && prb.prop == dnnl_backward) {
                      return args.diff < args.trh;
                  }
                  return false;
              };
    cmp.set_driver_check_function(rnn_add_check);

    SAFE(cmp.compare(mem_fp, mem_dt, prb.attr, res), WARN);
    return OK;
}

void prb_t::set_qparams(float fp_min, float fp_max) {
    if (!cfg.is_int8()) {
        data_shift = 0.;
        data_scale = 1.;
        wei_scales[0] = 1.;
        return;
    }

    /* Set parameters for quantization of src and weights from fp32 data
     * in [-1, 1] to int8 data in a range specified in cfg */
    float fp_range = fp_max - fp_min;
    float int8_src_range = cfg[SRC_LAYER].f_max - cfg[SRC_LAYER].f_min,
          int8_wei_range = cfg[WEIGHTS_LAYER].f_max - cfg[WEIGHTS_LAYER].f_min;

    // No shift needed for s8s8 AMX LSTM
    data_shift = cfg.is_s8() ? 0 : cfg[SRC_LAYER].f_mean;
    data_scale = int8_src_range / fp_range;

    float K = int8_wei_range / fp_range;
    auto set_wei_scales = [&](float *scales, int nelems) {
        for (int64_t i = 0; i < nelems; i++)
            scales[i] = K * (1. + (float)i / nelems);
    };

    set_wei_scales(wei_scales, wei_nscales);
    if (with_projection) set_wei_scales(wei_proj_scales, wei_proj_nscales);
}

void prb_t::set_tparams(float fp_min, float fp_max) {
    if (skip_nonlinear) {
        assert(linear_scales != nullptr);
        // Here, we assume that the inputs of the cells are in [fp_min,fp_max].
        // We pick the scaling factors to ensure that the output of the linear
        // pre/post gemm is in [fp_min,fp_max]

        // Also, we rely on the fact that for forward, the weights
        // matrices are sparse, and contain coefficient equal to
        // 1/n_gates() to compensate for the gemm accumulation. So
        // here, we account only for the post-gemm accumulation, and
        // the fact that we want to use different scales per gate.

        // For BWD_W, we cannot assume sparseness though since the
        // gates and diff_dst_* are dense.
        int64_t fwd_acc_dim = n_gates();
        int64_t bwdd_acc_dim = dhc;
        int64_t bwdw_acc_dim = mb;
        int64_t acc_dim = 0;
        if (prop == dnnl_backward)
            acc_dim = n_gates()
                    * MAX2(fwd_acc_dim, MAX2(bwdd_acc_dim, bwdw_acc_dim));
        else
            acc_dim = fwd_acc_dim;
        // make scaling exact by choosing powers of two.
        int64_t n_cscale = (alg == VANILLA_LSTM);
        int64_t divisor = next_pow2((acc_dim + n_cscale) * (is_int8() ? 2 : 1));
        float factor = (1.0f / (float)(divisor));
        for (int64_t i = 0; i < n_gates(); i++)
            linear_scales[i] = (i + 1) * factor;
        if (n_cscale) linear_cscale = (n_gates() + 1) * factor;
    }
}

} // namespace rnn
