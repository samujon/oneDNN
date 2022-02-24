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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sstream>

#include "dnnl_common.hpp"
#include "utils/parser.hpp"

#include "shuffle/graph_shuffle.hpp"
#include "shuffle/shuffle.hpp"

namespace shuffle {

void check_correctness(const settings_t &s) {
    for_(const auto &i_dir : s.dir)
    for_(const auto &i_dt : s.dt)
    for_(const auto &i_tag : s.tag)
    for_(const auto &i_group : s.group)
    for_(const auto &i_axis : s.axis)
    for (const auto &i_scratchpad_mode : s.scratchpad_mode) {
        attr_t attr;
        attr.insert(i_scratchpad_mode);

        const prb_t prb(s.prb_dims, i_dir, i_dt, i_tag, i_axis, i_group, attr);
        std::stringstream ss;
        ss << prb;
        const std::string cpp_pstr = ss.str();
        const char *pstr = cpp_pstr.c_str();
        BENCHDNN_PRINT(1, "run: %s\n", pstr);

        res_t res {};
        const int status = [&prb, &res](api_mode_t mode) {
            if (mode == PRIMITIVE)
                return doit(&prb, &res);
            else if (mode == GRAPH)
                return benchdnnext::shuffle::doit(&prb, &res);
            else
                return FAIL;
        }(api_mode);

        bool want_perf_report = false;
        parse_result(res, want_perf_report, status, pstr);

        if (want_perf_report && is_bench_mode(PERF)) {
            perf_report_t pr(&prb, s.perf_template);
            pr.report(&res, pstr);
        }

        benchdnn_stat.tests++;
    }
}

static const std::string help_group
        = "UINT    (Default: `1`)\n    Specifies number of elements to "
          "shuffle.\n";

int bench(int argc, char **argv) {
    driver_name = "shuffle";
    using namespace parser;
    static settings_t s;
    static const settings_t def {};
    for (; argc > 0; --argc, ++argv) {
        const bool parsed_options = parse_bench_settings(argv[0])
                || parse_batch(bench, argv[0])
                || parse_dir(s.dir, def.dir, argv[0])
                || parse_dt(s.dt, def.dt, argv[0])
                || parse_tag(s.tag, def.tag, argv[0])
                || parse_vector_option(
                        s.group, def.group, atoi, argv[0], "group", help_group)
                || parse_axis(s.axis, def.axis, argv[0])
                || parse_attr_scratchpad_mode(
                        s.scratchpad_mode, def.scratchpad_mode, argv[0])
                || parse_perf_template(s.perf_template, s.perf_template_def,
                        s.perf_template_csv(), argv[0])
                || parse_reset(s, argv[0]) || parse_help(argv[0]);
        if (!parsed_options) {
            catch_unknown_options(argv[0]);

            parse_prb_dims(s.prb_dims, argv[0]);
            check_correctness(s);
        }
    }

    return parse_last_argument();
}

} // namespace shuffle
