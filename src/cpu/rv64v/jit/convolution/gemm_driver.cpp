#include "common/utils.hpp"
#include "cpu/rv64v/jit/platform_traits.hpp"
#include "common/type_helpers.hpp"
#include "common/verbose.hpp"
#include "common/utils.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include "cpu/rv64v/jit/convolution/gemm_driver.hpp"
#include "cpu/rv64v/jit/convolution/gemm_kernel.hpp"
#include <iostream>
#include <cstdint>

namespace dnnl {
namespace impl {
namespace cpu {
namespace rv64 {
namespace gemm {

static float execution = 0;
// CPU engine implementation

uint64_t read_instret() {
    uint64_t instret;
    asm volatile("csrr %0, instret" : "=r"(instret));
    return instret;
}
struct schedule_factory_t {
    convolution_schedule_t &sched;
    kernel_traits_t *traits;
    size_t ntraits;
    size_t traits_capacity;
    size_t calls_capacity;
    static constexpr int alloc_traits = 32;//32;
    static constexpr int alloc_calls = 256;//256;

    schedule_factory_t(convolution_schedule_t &s) : sched(s) {
        traits = new kernel_traits_t[alloc_traits];
        s.calls = new convolution_schedule_t::pkernel_t[alloc_calls];
        s.args = new convolution_schedule_t::precalculated_args[alloc_calls];
        traits_capacity = alloc_traits;
        calls_capacity = alloc_calls;
        s.N = 0;
        s.NJ = 0;
    }

    virtual ~schedule_factory_t() {
        if (traits)
            delete [] traits;
        traits = nullptr;
        traits_capacity = 0;
    }
};

bool debugmode() {
    return getenv_int("DNNL_RV64_DEBUG", 0) != 0;
}

void print_config(const jit_convolution_configuration_t &cfg) {
    if (debugmode()) {
        #ifdef RVJ_VSPEC_1_0
        const char visa[] = "v1.0";
        #else
        const char visa[] = "v0.7";
        #endif
        printf("RISCV V ISA: %s\n", visa);
        printf("mb: %d\n", cfg.mb);
        printf("mb: %d\n", cfg.mb);
        printf("oh: %d\n", cfg.oh);
        printf("ow: %d\n", cfg.ow);
        printf("ih: %d\n", cfg.ih);
        printf("iw: %d\n", cfg.iw);
        printf("kh: %d\n", cfg.kh);
        printf("kw: %d\n", cfg.kw);
        printf("oc: %d\n", cfg.oc);
        printf("ic: %d\n", cfg.ic);
        printf("ngroups: %d\n", cfg.ngroups);
        printf("prop_kind: %d\n", cfg.prop_kind);
        printf("src_dt: %d\n", cfg.src_dt);
        printf("wei_dt: %d\n", cfg.wei_dt);
        printf("dst_dt: %d\n", cfg.dst_dt);
        printf("bias_dt: %d\n", cfg.bias_dt);
        printf("with_bias: %d\n", cfg.with_bias);
        printf("stride_h: %d\n", cfg.stride_h);
        printf("stride_w: %d\n", cfg.stride_w);
        printf("l_pad: %d\n", cfg.l_pad);
        printf("t_pad: %d\n", cfg.t_pad);
        printf("dilate_h: %d\n", cfg.dilate_h);
        printf("dilate_w: %d\n", cfg.dilate_w);
        printf("maxvl: %d\n", cfg.maxvl);
        printf("vcores: %d\n", cfg.vcores);
        printf("vcore_shared_cache_size: %d\n", cfg.vcore_shared_cache_size);
        printf("l1d_cache_size: %d\n", cfg.l1d_cache_size);
        printf("l1d_cache_line_size: %d\n", cfg.l1d_cache_line_size);
        printf("nvregs: %d\n", cfg.nvregs);
        printf("icb: %d\n", cfg.icb);
        printf("ocb: %d\n", cfg.ocb);
        printf("w_icb: %d\n", cfg.w_icb);
        printf("w_ocb: %d\n", cfg.w_ocb);
        printf("w_dim_permute: [%d, %d, %d, %d]\n",
            cfg.w_dim_permute[0], cfg.w_dim_permute[1],
            cfg.w_dim_permute[2], cfg.w_dim_permute[3]);
        printf("w_inner_blocks: [%d, %d]\n",
            cfg.w_inner_blocks[0], cfg.w_inner_blocks[1]);
        printf("w_inner_idx: [%d, %d]\n",
            cfg.w_inner_idx[0], cfg.w_inner_idx[1]);
        printf("vdim_is_oc: %d\n", cfg.vdim_is_oc);
        printf("vlen: %d\n", cfg.vlen);
        printf("rbw: %d\n", cfg.rbw);
        printf("rbc: %d\n", cfg.rbc);
        printf("k_c: %d\n", cfg.k_c);
        printf("k_h: %d\n", cfg.k_h);
        printf("k_w: %d\n", cfg.k_w);
    }
}

void dump_jit_code_to_file(rvjit::function* f, const char *ofname) {
    if (!f || !ofname || strlen(ofname) == 0)
        return;
    size_t s = f->size();
    if (s == 0)
        return;
    char *buf = new char[s];
    if (f->dump(buf) == s) {
        std::ofstream o;
        o.open(ofname, std::ofstream::binary);
        o.write(buf, f->size());
        o.close();
    }
    delete [] buf;
}

status_t fill_blocked(memory_desc_t &md, std::initializer_list<int> perm,
        std::initializer_list<int> inner_blks,
        std::initializer_list<int> inner_idxs) {
    const bool ok = true && perm.size() == (size_t)md.ndims
            && inner_blks.size() == inner_idxs.size();
    if (!ok) return status::invalid_arguments;

    md.offset0 = 0;

    blocking_desc_t &blk = md.format_desc.blocking;

    dim_t block_size = 1;
    dims_t blocks = {0};
    utils::array_set(blocks, 1, md.ndims);

    blk.inner_nblks = (int)inner_blks.size();

    int iblk = 0;
    for (const auto &b : inner_idxs)
        blk.inner_idxs[iblk++] = b;

    iblk = 0;
    for (const auto &b : inner_blks) {
        int dim = blk.inner_idxs[iblk];
        block_size *= b;
        blocks[dim] *= b;
        blk.inner_blks[iblk++] = b;
    }

    utils::array_set(md.padded_offsets, 0, md.ndims);
    for (int d = 0; d < md.ndims; ++d)
        md.padded_dims[d] = utils::rnd_up(md.dims[d], blocks[d]);

    // setting the strides
    {
        dim_t stride = block_size;
        auto iter_d = perm.end(); // reverse iterator over perm
        do {
            const int d = *(--iter_d);
            blk.strides[d] = stride;

            const dim_t pdim = md.padded_dims[d];
            if (utils::one_of(DNNL_RUNTIME_DIM_VAL, stride, pdim))
                stride = DNNL_RUNTIME_DIM_VAL;
            else if (pdim != 0)
                stride *= pdim / blocks[d];

        } while (iter_d != perm.begin());
    }

    return status::success;
}

int log2(int x) {
    int r = 0;
    if (x < 1)
        return -1;
    do {
        if (x == 1)
            return r;
        if (x%2)
            return -1;
        x = x / 2;
        ++r;
    } while (true);
}

void schedule_iteration(schedule_factory_t &f,
                        kernel_traits_t t,
                        convolution_schedule_t::precalculated_args &a) {

    convolution_schedule_t::jit_conv_kernel_args_t kargs;

    rvjit::function* handle;
    auto &s = f.sched;
    size_t id = s.NJ;
    // Check if a kernel exists that maps to the current traits structure
    for (size_t i = 0; i < s.NJ; ++i) {
        if (t == f.traits[i]) {
            id = i;
            break;
        }
    }
    // Bookeeping when creating a new kernel that solve this subconvolution
    if (id == s.NJ) {
        jit_convolution_kernel_t ker = jit_convolution_kernel_t(s.cfg, t);
        if (s.NJ >= f.traits_capacity) {
            f.traits_capacity += f.alloc_traits;
            auto old_traits = f.traits;
            f.traits = new kernel_traits_t[f.traits_capacity];
            for (size_t i = 0; i < s.N; ++i)
                f.traits[i] = old_traits[i];
            delete [] old_traits;
        }
        id = s.NJ;
        ker.code(kargs);
        s.handles[id] = ker.assemble();
        f.traits[id] = t;
        ++s.NJ;
        if (debugmode()) {
            char name[32];
            snprintf(name, 32, "kernel_%ld.bin", id);
            dump_jit_code_to_file(s.handles[id], name);
            printf("Creating kernel %s (%p) with traits: ",
                name, s.handles[id]->get());
           
        }
    }
    handle = s.handles[id];
    // Schedule subconvolution by pre-calculating the kernel arguments
    if (s.N >= f.calls_capacity) {
        f.calls_capacity += f.alloc_calls;
        auto old_calls = s.calls;
        s.calls = new convolution_schedule_t::pkernel_t[f.calls_capacity];
        for (size_t i = 0; i < s.N; ++i)
            s.calls[i] = old_calls[i];
        delete [] old_calls;

        auto old_args = s.args;
        s.args = new convolution_schedule_t::precalculated_args[f.calls_capacity];
        for (size_t i = 0; i < s.N; ++i)
            s.args[i] = old_args[i];
        delete [] old_args;
    }
    s.calls[s.N] = handle->get<convolution_schedule_t::pkernel_t>();
    s.args[s.N].load_partials = a.load_partials;
    s.args[s.N].h_loop_size = a.h_loop_size;
    s.args[s.N].w_loop_size = a.w_loop_size;
    s.args[s.N].vlen = a.vlen;
    s.args[s.N].dst = a.dst;
    s.args[s.N].src = a.src;
    s.args[s.N].wei = a.wei;
    s.args[s.N].bias = a.bias;
    ++s.N;

}


bool init_conf(jit_convolution_configuration_t &out,
               const convolution_desc_t &cd, const memory_desc_t &dst_md,
               const memory_desc_t &src_md, const memory_desc_t &wei_md,
               const memory_desc_t &bias_md) {
    const memory_desc_wrapper src_d(&src_md);
    const memory_desc_wrapper dst_d(&dst_md);
    const memory_desc_wrapper wei_d(&wei_md);
    const memory_desc_wrapper bias_d(&bias_md);
    const bool with_groups = wei_d.ndims() == src_d.ndims() + 1;
    const int ndims = src_d.ndims();

    // Check if implementation is disabled
    if (!getenv_int("DNNL_RV64_ENABLE_VOPS", 1))
        return false;

    // Populate convolution arguments
    out.prop_kind = cd.prop_kind;
    out.with_bias = cd.bias_desc.format_kind != format_kind::undef;
    out.src_dt = src_d.data_type();
    out.wei_dt = wei_d.data_type();
    out.bias_dt = bias_d.data_type();
    out.dst_dt = dst_d.data_type();
    out.ngroups = with_groups ? wei_d.dims()[0] : 1;
    out.stride_h = (ndims == 3) ? 1 : cd.strides[ndims - 4];
    out.stride_w = cd.strides[ndims - 3];
    out.dilate_h = (ndims == 3) ? 0 : cd.dilates[ndims - 4];
    out.dilate_w = cd.dilates[ndims - 3];
    out.t_pad = (ndims == 3) ? 0 : cd.padding[0][ndims - 4];
    out.l_pad = cd.padding[0][ndims - 3];
    out.mb = src_d.dims()[0];
    out.oc = dst_d.dims()[1] / out.ngroups;
    out.ic = src_d.dims()[1] / out.ngroups;
    out.ih = (ndims == 3) ? 1 : src_d.dims()[ndims - 2];
    out.iw = src_d.dims()[ndims - 1];
    out.oh = (ndims == 3) ? 1 : dst_d.dims()[ndims - 2];
    out.ow = dst_d.dims()[ndims - 1];
    out.kh = (ndims == 3) ? 1 : wei_d.dims()[with_groups + ndims - 2];
    out.kw = wei_d.dims()[with_groups + ndims - 1];
    // Populate hardware parameters
    out.maxvl = get_platform_maxvl() / (types::data_type_size(out.dst_dt) * 8);
    out.vcores = get_platform_vcores();
    out.vcore_shared_cache_size = get_platform_vector_level_cache_size();
    out.l1d_cache_size = get_platform_l1d_cache_size();
    out.l1d_cache_line_size = get_platform_l1d_cache_line_size();
    out.nvregs = get_platform_vregs_count();

    int const dt_size = types::data_type_size(out.dst_dt);
    int const nelems_per_cache_line = out.l1d_cache_line_size / dt_size;

    // Sanity checks and current implementation limitations
    bool sane = utils::everyone_is(true,
        // Grouped convolution still not supported
        out.ngroups == 1,
        // Dilated convolution still not supported
        out.dilate_h == out.dilate_w && out.dilate_h == 0,
        // Large padding still not supported
        out.l_pad == out.t_pad && out.l_pad <= 2,
        // Spatial convolution with stride not supported
        (out.kh * out.kw == 1 || out.stride_h * out.stride_w == 1),
        // Irregular division of vlen/oc or vlen/ic not handled
        log2(out.oc) >= 0 && log2(out.ic) >= 0
    );
    if (!sane)
        return false;
    
    // Decide on software optimizations
    // 1. Define vectorization strategy and vector length
    switch (out.prop_kind) {
        case prop_kind::forward_inference: {
            out.vdim_is_oc = true;
            out.vlen = out.maxvl;//nstl::min(out.oc, out.maxvl);
            break;
        }
        case prop_kind::forward: {
            out.vdim_is_oc = true;
            out.vlen = out.maxvl;//nstl::min(out.oc, out.maxvl);
            break;
        }
        default:
            return false;
    }
    
    // 2. Declare the tensor memory layout
    out.icb = nstl::min(out.ic, out.maxvl);
    out.ocb = nstl::min(out.oc, out.maxvl);
    if (out.vdim_is_oc) {
        out.w_ocb = out.vlen;
        out.w_icb = nstl::min(nelems_per_cache_line, out.ic);
        out.w_dim_permute[0] = 0;
        out.w_dim_permute[1] = 1;
        out.w_inner_blocks[0] = out.w_icb;
        out.w_inner_blocks[1] = out.w_ocb;
        out.w_inner_idx[0] = 1;
        out.w_inner_idx[1] = 0;
    } else {
        out.w_ocb = nstl::min(nelems_per_cache_line, out.oc);
        out.w_icb = out.vlen;
        out.w_dim_permute[0] = 1;
        out.w_dim_permute[1] = 0;
        out.w_inner_blocks[0] = out.w_ocb;
        out.w_inner_blocks[1] = out.w_icb;
        out.w_inner_idx[0] = 0;
        out.w_inner_idx[1] = 1;
    }
    out.w_dim_permute[2] = 2;
    out.w_dim_permute[3] = 3;

    /// 3. Define the register block dimensions (blocking of spatial loops)
    /// Create independent accumulation chains to hide vector FMA latency.
    /// Also, avoid conflict misses on L1 cache on long vector lengths.
    /// Maximum register block size that does not cause L1D conflicts

    /// Channels on FMA source tensor accessed with scalar instructions (bcast)
    const int c = out.vdim_is_oc ? out.icb : out.ocb;

    out.rbc = 1;
    out.rbw = 1;

    if (out.prop_kind != prop_kind::backward_weights) {
        /// Unroll factor limit as the smallest unroll deterrent between the
        /// register file size or presence of L1D cache conflict misses.
        /// The latter originate from the observed algorithm memory access
        /// pattern to the tensor that populates the vector FMA scalar operand
        /// (src on FWDD, dst on BWDD, src/dst on BWDW).
        int const upper_limit = nstl::min(out.nvregs - 1,
            out.l1d_cache_size / (c*dt_size));
        
        out.rbw = out.vdim_is_oc ? out.ow : out.iw / out.stride_w;
        while (out.rbw > 1 && out.rbw > upper_limit)
            out.rbw = out.rbw/2 + (out.rbw%2 > 0);
    } else {
        int const upper_limit = out.nvregs - 1;
        out.rbc = nstl::min(c, nelems_per_cache_line);
        while (out.rbc > 1 && out.rbc > upper_limit)
            out.rbc = out.rbc/2 + (out.rbc%2 > 0);
    }

    // 4. Decide the kernel granularity so that weights fit in cache
    if (out.prop_kind != prop_kind::backward_weights) {
        auto footprint = [&]() {
            int f = out.vlen * out.k_c * out.k_h * out.k_w; // wei sub-tensor
            f += out.vlen * out.rbw;                        // dst sub-tensor
            f += out.k_c  * out.kh * out.rbw;               // src sub-tensor
            return f * dt_size;
        };

        out.k_c = c;
        out.k_h = out.kh;
        out.k_w = out.kw;
        while (out.vcore_shared_cache_size < footprint()) {
            if (out.k_c > nelems_per_cache_line) {
                out.k_c /= 2;
            } else if (out.k_h > 1) {
                out.k_h = 1;
                out.k_c = c;
            } else if (out.k_w > 1) {
                out.k_w = 1;
                out.k_c = c;
            } else {
                out.k_c = nstl::min(nelems_per_cache_line, c);
                break;
            }
        } 
    } else {
        auto const vh = out.vdim_is_oc ? out.oh : out.ih / out.stride_h;
        auto const vw = out.vdim_is_oc ? out.ow : out.iw / out.stride_w;
        auto footprint = [&]() {
            int f = out.vlen * out.k_c * out.rbw; // wei tensor
            f += out.vlen * out.k_h * out.k_w;    // vector src
            f += out.k_c  * out.k_h * out.k_w;    // scalar src
            return f * dt_size;
        };

        out.k_c = nstl::min(nelems_per_cache_line, c);
        out.k_h = vh;
        out.k_w = vw;
        while (out.vcore_shared_cache_size < footprint() && out.k_h > 1)
            out.k_h /= 2 + (out.k_h%2 > 0);
    }
    
    // Check if user-supplied src format is compatible with algorithm
    // nChwXc memory format is compatible when X is out.icb
    if (src_d.format_kind() != format_kind::any) {
        if (src_d.format_kind() != format_kind::blocked)
            return false;
        auto bd = src_d.blocking_desc();
        bool ok = false;
        if (bd.inner_nblks == 0)
            ok = bd.strides[1] == 1 && out.ic == out.icb;
        else if (bd.inner_nblks == 1)
            ok = bd.inner_idxs[0] == 1 && bd.inner_blks[0] == out.icb;
        
        if (!ok)
            return false;
    }

    // Check if user-supplied dst format is compatible with algorithm.
    // nChwXc memory format is compatible when X is out.ocb
    if (dst_d.format_kind() != format_kind::any) {
        auto bd = dst_d.blocking_desc();
        bool ok = false;

        if (dst_d.format_kind() != format_kind::blocked)
            return false;

        if (bd.inner_nblks == 0)
            ok = bd.strides[1] == 1 && out.oc == out.ocb;
        else if (bd.inner_nblks == 1)
            ok = bd.inner_idxs[0] == 1 && bd.inner_blks[0] == out.ocb;

        if (!ok)
            return false;
    }

    // Check if user-supplied wei format is compatible with algorithm
    // When out.vdim_is_oc is true, the algorithm uses the IOhwXiYo format
    // When out.vdim_is_oc is false, the algorithm uses the OIhwXoYi format
    if (wei_d.format_kind() != format_kind::any) {
        auto bd = wei_d.blocking_desc();
        bool ok = false;

        if (wei_d.format_kind() != format_kind::blocked)
            return false;

        int x  = out.oc;
        int xb = out.ocb;
        int y  = out.ic;
        int yb = out.icb;
        if (out.vdim_is_oc) {
            x  = out.ic;
            xb = out.icb;
            y  = out.oc;
            yb = out.ocb;
        }

        if (bd.inner_nblks == 0)
            ok = bd.strides[!out.vdim_is_oc] == 1
                && bd.strides[out.vdim_is_oc] == yb
                && y == yb
                && x == xb;
        else if (bd.inner_nblks == 2)
            ok = bd.inner_idxs[0] == out.vdim_is_oc
                && bd.inner_idxs[1] != out.vdim_is_oc
                && bd.inner_blks[0] == xb
                && bd.inner_blks[1] == yb;
        
        if (!ok)
            return false;
    }

    return true;
}

bool pick_memory_formats_from_conf(const jit_convolution_configuration_t &cfg,
                                   memory_desc_t &dst_md,
                                   memory_desc_t &src_md,
                                   memory_desc_t &wei_md,
                                   memory_desc_t &bias_md) {
    if (dst_md.format_kind == format_kind::any) {
        CHECK(fill_blocked(dst_md, {0, 1, 2, 3}, {cfg.ocb}, {1}));
        dst_md.format_kind = format_kind::blocked;
    }
    if (src_md.format_kind == format_kind::any) {
        CHECK(fill_blocked(src_md, {0, 1, 2, 3}, {cfg.icb}, {1}));
        src_md.format_kind = format_kind::blocked;
    }
    if (wei_md.format_kind == format_kind::any) {
        CHECK(fill_blocked(wei_md,
            {cfg.w_dim_permute[0], cfg.w_dim_permute[1],
                cfg.w_dim_permute[2], cfg.w_dim_permute[3]},
            {cfg.w_inner_blocks[0], cfg.w_inner_blocks[1]},
            {cfg.w_inner_idx[0], cfg.w_inner_idx[1]})
        );
        wei_md.format_kind = format_kind::blocked;
    }
    if (bias_md.format_kind == format_kind::any)
        memory_desc_init_by_tag(bias_md, format_tag::a);

    return true;
}

void init_schedule(convolution_schedule_t &s,
                    const jit_convolution_configuration_t &acfg) {
    s.cfg = acfg;
    auto schedule = schedule_factory_t(s);
    kernel_traits_t t;
    convolution_schedule_t::precalculated_args a = s.args[0];
    print_config(acfg);
    switch (acfg.prop_kind) {
        case prop_kind::forward: {
            //schedule_fwdd(s);
            //ker.code(kargs);
            //ker.assemble();
            //schedule_iteration(schedule, t, a);
            break;
        }
        case prop_kind::forward_inference: {
            //schedule_fwdd(s);
            schedule_iteration(schedule, t, a);
            break;
        }
        default:
            assert(!"unsupported propagation kind");
            break;
    }
}

void free_schedule(convolution_schedule_t &s) {
    if (s.N > 0) {
        delete [] s.calls;
        delete [] s.args;
    }
    for (size_t i = 0; i < s.NJ; ++i)
        delete s.handles[i];
    s.N = 0;
    s.NJ = 0;
}

void call_schedule(const convolution_schedule_t &s, int i, int mb,
const float *dst, const float *src, const float *wei, const float *bias, const float *inter, const float *inter2) {
    //std::cout << "Executing layer " << i + 1 << std::endl;
    uint64_t start_instret = read_instret();


    std::cout << "call_schedule" << std::endl;
    convolution_schedule_t::jit_conv_kernel_args_t kargs;
    convolution_schedule_t::precalculated_args &args = s.args[i];
    kargs.dst = dst + args.dst;
    kargs.src = src + args.src;
    kargs.wei = wei + args.wei;
    kargs.bias = bias + args.bias;
    kargs.inter = inter;
    kargs.inter2 = inter2;
    kargs.vlen = args.vlen;
    kargs.h_loop_size = args.h_loop_size;
    kargs.w_loop_size = args.w_loop_size;
    kargs.load_partials = args.load_partials
        || (s.cfg.prop_kind == prop_kind::backward_weights ? mb > 0 : false);
    s.calls[i](kargs);
    uint64_t end_instret = read_instret();

    std::cout << "Instructions executed: " << (end_instret - start_instret) << std::endl;
}

} // namespace gemm
} // namespace dnnl
} // namespace impl
} // namespace cpu
} // namespace riscvv