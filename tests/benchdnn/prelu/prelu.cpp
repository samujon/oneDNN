/*******************************************************************************
* Copyright 2020 Intel Corporation
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

#include <math.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>

#include "dnnl.h"

#include "tests/test_thread.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"

#include "prelu/prelu.hpp"

namespace prelu {

static int init_pd(dnnl_engine_t engine, const prb_t *prb,
        dnnl_primitive_desc_t &ppd, res_t *res, dir_t dir,
        const_dnnl_primitive_desc_t hint) {
    dnnl_prelu_desc_t pd;
    dnnl_memory_desc_t data_d, weights_d;

    const auto &src_dims = prb->sdims[0];
    const auto &weight_dims = prb->sdims[1];

    SAFE(init_md(&data_d, prb->ndims, src_dims.data(), prb->sdt[0],
                 prb->stag[0]),
            CRIT);
    SAFE(init_md(&weights_d, prb->ndims, weight_dims.data(), prb->sdt[1],
                 prb->stag[1]),
            CRIT);

    if (prb->dir & FLAG_FWD) {
        auto prop = prb->dir & FLAG_INF ? dnnl_forward_inference
                                        : dnnl_forward_training;
        DNN_SAFE(dnnl_prelu_forward_desc_init(&pd, prop, &data_d, &weights_d),
                WARN);
    } else {
        dnnl_memory_desc_t diff_data_d, diff_weights_d;
        SAFE(init_md(&diff_data_d, prb->ndims, src_dims.data(), prb->sdt[0],
                     prb->stag[0]),
                CRIT);
        SAFE(init_md(&diff_weights_d, prb->ndims, weight_dims.data(),
                     prb->sdt[0], prb->stag[1]),
                CRIT);

        DNN_SAFE(dnnl_prelu_backward_desc_init(&pd, &data_d, &weights_d,
                         &diff_data_d, &diff_weights_d),
                WARN);
    }

    auto dnnl_attr = create_dnnl_attr(prb->attr, attr_args_t());

    dnnl_status_t init_status
            = dnnl_primitive_desc_create(&ppd, &pd, dnnl_attr, engine, nullptr);

    dnnl_primitive_attr_destroy(dnnl_attr);

    if (init_status == dnnl_unimplemented)
        return res->state = UNIMPLEMENTED, OK;
    SAFE(init_status, WARN);

    res->impl_name = query_impl_info(ppd);
    BENCHDNN_PRINT(5, "oneDNN implementation: %s\n", res->impl_name.c_str());

    return OK;
}

// Check that on a given input specific alg may return NaN or inf.
static bool check_extreme_values(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isinf(a) && std::isinf(b) && std::signbit(a) == std::signbit(b))
        return true;
    return false;
}

static int compare(const prb_t *prb, data_kind_t kind, const dnn_mem_t &mem_fp,
        const dnn_mem_t &mem_dt, res_t *res) {
    const auto nelems = mem_dt.nelems();
    const auto trh = 2 * epsilon_dt(prb->sdt[0]);

    if (nelems == 0) return res->state = PASSED, OK;

    res->total = nelems;

    for (int64_t i = 0; i < nelems; i++) {
        const float dt = mem_dt.get_elem(i);
        const float fp0 = mem_fp.get_elem(i);
        const float fp = round_to_nearest_representable(prb->sdt[0], fp0);
        const float diff = fabsf(fp - dt);
        const float rel_diff = diff / (fabsf(fp) > FLT_MIN ? fabsf(fp) : 1);

        bool ok = (fabsf(fp) > 1e-5 ? rel_diff : diff) <= trh;
        if (!ok) ok = check_extreme_values(fp, dt);

        res->errors += !ok;

        const bool dump = (!ok && (res->errors < 10 || verbose >= 10))
                || (verbose >= 50 && i < 30) || (verbose >= 99);
        if (dump) {
            std::stringstream ss;
            dims_t dims_idx = (kind == WEI) ? off2dims_idx(prb->sdims[1], i)
                                            : off2dims_idx(prb->sdims[0], i);
            ss << dims_idx;
            std::string ind_str = ss.str();

            BENCHDNN_PRINT(0,
                    "[%4ld][%s][%s] fp0:% 9.6g fp:% 9.6g dt:% 9.6g "
                    "diff:%8.3g rdiff:%8.3g\n",
                    (long)i, data_kind2str(kind), ind_str.c_str(), fp0, fp, dt,
                    diff, rel_diff);
        }
    }

    if (res->errors) res->state = FAILED;

    if (res->state == UNTESTED) res->state = PASSED; /* optimism */

    return res->state == FAILED ? FAIL : OK;
}

int fill_data(const prb_t *prb, data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp) {
    const auto nelems = mem_fp.nelems();
    if (nelems == 0) return OK;

    // Do fixed partitioning to have same filling for any number of threads.
    const int64_t n_chunks = 16;
    const int64_t chunk_size = div_up(nelems, n_chunks);
    // Current filling will not work for int data types
    assert(!is_integral_dt(mem_dt.dt()));

    dnnl::impl::parallel_nd(n_chunks, [&](int idx_chunk) {
        int64_t idx_start = idx_chunk * chunk_size;
        int64_t idx_end = MIN2(idx_start + chunk_size, nelems);
        // Note 1: we use a different seed for each chunk to avoid
        // repeating patterns. We could use discard(idx_start) too but
        // we avoid it for two reasons:
        //   a. it has a complexity in O(idx_start).
        //   b. igen and fgen below might require more than 1 sample
        //   per idx, so the we cannot deterministically compute the
        //   number of states we need to discard
        // Note 2: We also advance the state to avoid having only
        // small values as first chunk input.  The +1 is necessary to
        // avoid generating zeros in first chunk.
        // Note 3: we multiply by kind + 1 to have different values in
        // src/dst and diff_dst. The +1 is to avoid 0 again.
        std::minstd_rand msr((idx_start + 1) * (kind + 1));
        msr.discard(1);
        std::uniform_int_distribution<> igen(0, 2);
        std::uniform_real_distribution<> fgen(-1.f, 1.f);
        for (int64_t idx = idx_start; idx < idx_end; ++idx) {
            float value = kind == SRC
                    ? igen(msr)
                    : kind == WEI ? fgen(msr) : igen(msr) / 8.f;
            // TODO: amount of negative values should depend on number of points
            // to reduce as summation becomes inaccurate.
            float sign = flip_coin(idx, 0.1f) ? -1.f : 1.f;
            value = round_to_nearest_representable(mem_dt.dt(), sign * value);
            mem_fp.set_elem(idx, value);
        }
    });

    SAFE(mem_dt.reorder(mem_fp), WARN);

    return OK;
}

void check_known_skipped_case(const prb_t *prb, res_t *res) {
    check_known_skipped_case_common(prb->sdt, FWD_D, res);
    if (res->state == SKIPPED) return;

    if (is_cpu()) {
        if (prb->sdt[0] != prb->sdt[1]) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
            return;
        }
    }
}

int doit(const prb_t *prb, res_t *res) {
    if (bench_mode == LIST) return res->state = LISTED, OK;

    check_known_skipped_case(prb, res);
    if (res->state == SKIPPED) return OK;

    dnnl_primitive_t p {};
    SAFE(init_prim(&p, init_pd, prb, res), WARN);
    if (res->state == SKIPPED || res->state == UNIMPLEMENTED) return OK;

    const_dnnl_primitive_desc_t const_pd;
    DNN_SAFE(dnnl_primitive_get_primitive_desc(p, &const_pd), CRIT);

    if (dnn_mem_t::check_mem_size(const_pd) != OK) {
        DNN_SAFE_V(dnnl_primitive_destroy(p));
        return res->state = SKIPPED, res->reason = NOT_ENOUGH_RAM, OK;
    }

    const auto q = [&](int index = 0) -> const dnnl_memory_desc_t & {
        return *dnnl_primitive_desc_query_md(
                const_pd, dnnl_query_exec_arg_md, index);
    };

    const auto &data_md = q(DNNL_ARG_SRC);
    const auto &weight_md = q(DNNL_ARG_WEIGHTS);
    const auto &scratchpad_md = q(DNNL_ARG_SCRATCHPAD);
    const auto &test_engine = get_test_engine();

    dnn_mem_t src_fp(data_md, dnnl_f32, tag::abx, test_engine);
    dnn_mem_t weights_fp(weight_md, dnnl_f32, tag::abx, test_engine);

    dnn_mem_t src_dt(data_md, test_engine);
    dnn_mem_t weights_dt(weight_md, test_engine);
    dnn_mem_t scratchpad_dt(scratchpad_md, test_engine);

    SAFE(fill_data(prb, SRC, src_dt, src_fp), WARN);
    SAFE(fill_data(prb, WEI, weights_dt, weights_fp), WARN);

    args_t args;
    args.set(DNNL_ARG_SRC, src_dt);
    args.set(DNNL_ARG_WEIGHTS, weights_dt);
    args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

    dnn_mem_t dst_dt, d_src_fp, d_src_dt, d_dst_fp, d_dst_dt, d_weights_fp,
            d_weights_dt;

    if (prb->dir & FLAG_FWD) {
        dnn_mem_t dst_fp(data_md, dnnl_f32, tag::abx, test_engine);
        dst_dt = dnn_mem_t(data_md, test_engine);

        args.set(DNNL_ARG_DST, dst_dt);
        SAFE(execute_and_wait(p, args), WARN);

        if (bench_mode & CORR) {
            compute_ref_fwd(prb, src_fp, weights_fp, dst_fp);
            dnn_mem_t dst(dst_dt, dnnl_f32, tag::abx, test_engine);
            SAFE(compare(prb, SRC, dst_fp, dst, res), WARN);
        }
    } else {
        const auto &d_data_md = q(DNNL_ARG_DIFF_DST);
        const auto &d_weights_md = q(DNNL_ARG_DIFF_WEIGHTS);

        dnn_mem_t d_src_fp(d_data_md, dnnl_f32, tag::abx, test_engine);
        dnn_mem_t d_weights_fp(d_weights_md, dnnl_f32, tag::abx, test_engine);
        dnn_mem_t d_dst_fp(d_data_md, dnnl_f32, tag::abx, test_engine);

        d_src_dt = dnn_mem_t(d_data_md, test_engine);
        d_weights_dt = dnn_mem_t(d_weights_md, test_engine);
        d_dst_dt = dnn_mem_t(d_data_md, test_engine);

        SAFE(fill_data(prb, DST, d_dst_dt, d_dst_fp), WARN);

        args.set(DNNL_ARG_DIFF_DST, d_dst_dt);
        args.set(DNNL_ARG_DIFF_SRC, d_src_dt);
        args.set(DNNL_ARG_DIFF_WEIGHTS, d_weights_dt);
        SAFE(execute_and_wait(p, args), WARN);

        if (bench_mode & CORR) {
            compute_ref_bwd(
                    prb, src_fp, weights_fp, d_src_fp, d_dst_fp, d_weights_fp);

            dnn_mem_t d_src(d_src_dt, dnnl_f32, tag::abx, test_engine);
            SAFE(compare(prb, SRC, d_src_fp, d_src, res), WARN);

            dnn_mem_t d_weights(d_weights_dt, dnnl_f32, tag::abx, test_engine);
            SAFE(compare(prb, WEI, d_weights_fp, d_weights, res), WARN);
        }
    }

    measure_perf(res->timer, p, args);

    DNN_SAFE_V(dnnl_primitive_destroy(p));

    return OK;
}

} // namespace prelu
