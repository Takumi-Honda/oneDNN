/*******************************************************************************
* Copyright 2019-2021 Intel Corporation
* Copyright 2020-2021 FUJITSU LIMITED
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

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/nstl.hpp"
#include "common/utils.hpp"

#include "cpu/aarch64/injectors/jit_uni_eltwise_injector.hpp"

#define IDX(a) static_cast<uint32_t>(a.getIdx())

namespace dnnl {
namespace impl {
namespace cpu {
namespace aarch64 {

using namespace Xbyak_aarch64;

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::injector_preamble(
        const injector_utils::vmm_index_set_t &vmm_idxs) {
    using namespace alg_kind;
    using namespace Xbyak_aarch64::util;
    preserved_vecs_count = 0;
    vecs_to_preserve = aux_vecs_count();
    const auto start_idx = *(vmm_idxs.begin());
    const auto end_idx = *(vmm_idxs.rbegin()) + 1;
    start_idx_tail = vmm_idxs.begin();

    // For sse41 mask register has to be Xmm(0)
    if (isa == asimd && vecs_to_preserve > 0) {
        size_t idx = 0;
        assert(idx < start_idx);
        preserved_vec_idxs[preserved_vecs_count++] = idx;
    }

    for (size_t idx = preserved_vecs_count; idx < vecs_count; idx++) {
        if (preserved_vecs_count >= vecs_to_preserve) break;
        if (start_idx <= idx && idx < end_idx) continue;

        preserved_vec_idxs[preserved_vecs_count++] = idx;
    }

    size_t preserved_vecs_count_tail = vecs_to_preserve - preserved_vecs_count;
    for (size_t i = 0; i < preserved_vecs_count_tail; i++) {
        preserved_vec_idxs[preserved_vecs_count++] = *start_idx_tail;
        ++start_idx_tail;
    }

    assert(preserved_vecs_count == vecs_to_preserve);

    // Same logic but to allocate gprs
    size_t preserved_gprs_count = 0;
    for (size_t gpr_idx = 0; gpr_idx <= 30; ++gpr_idx) {
        int _idx = 30 - gpr_idx; // we allocate from the end
        if (preserved_gprs_count < aux_gprs_count()
                && (((unsigned)_idx) != x_table.getIdx()))
            preserved_gpr_idxs[preserved_gprs_count++] = _idx;
    }
    assert(preserved_gprs_count == aux_gprs_count());

    h->xa_->ptrue(p_512.b);

    if (save_state_) {
        h->xa_->str(x_table, pre_ptr(h->X_SP, -8));

        for (size_t i = 0; i < preserved_gprs_count; ++i) {
            /* This route has not been tested */
            h->xa_->str(XReg(preserved_gpr_idxs[i]), pre_ptr(h->X_SP, -8));
        }

        if (preserved_vecs_count)
            h->xa_->sub_imm(
                    h->X_SP, h->X_SP, preserved_vecs_count * vlen, h->X_TMP_0);

        size_t i = 0;

        while (i < preserved_vecs_count) {
            int count = 0;
            int ii = i;
            do {
                h->xa_->add_imm(h->x_tmp_vec[count++], h->X_SP, i * vlen,
                        h->X_DEFAULT_ADDR);
                i++;
            } while (i < preserved_vecs_count && count < h->x_tmp_vec_size);

            for (int j = 0; j < count; j++)
                h->xa_->st1w(ZRegS(preserved_vec_idxs[ii++]), p_512,
                        ptr(h->x_tmp_vec[j]));
        }
        load_table_addr();
    }

    assign_regs();
    set_coef_to_regs();
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::injector_preamble_tail(
        const injector_utils::vmm_index_set_iterator_t start_idx_it) {
    size_t tail_vecs_to_preserve = std::distance(start_idx_it, start_idx_tail);
    if (tail_vecs_to_preserve == 0) return;

    const int idx_off = vecs_to_preserve - tail_vecs_to_preserve;

    if (save_state_) {
        /* This route has not been tested */
        if (idx_off)
            h->xa_->add_imm(h->X_SP, h->X_SP, idx_off * vlen, h->X_TMP_0);

        size_t i = 0;

        while (i < tail_vecs_to_preserve) {
            int count = 0;
            int ii = i;
            do {
                h->xa_->add_imm(h->x_tmp_vec[count++], h->X_SP, i * vlen,
                        h->X_DEFAULT_ADDR);
                i++;
            } while (i < tail_vecs_to_preserve && count < h->x_tmp_vec_size);

            for (int j = 0; j < count; j++)
                h->xa_->ld1w(ZRegS(preserved_vec_idxs[idx_off + ii++]),
                        p_512 / T_z, ptr(h->x_tmp_vec[j]));
        }
    }

    for (size_t i = 0; i < tail_vecs_to_preserve; ++i)
        preserved_vec_idxs[idx_off + i] += tail_vecs_to_preserve;

    if (save_state_) {
        size_t i = 0;

        while (i < tail_vecs_to_preserve) {
            int count = 0;
            int ii = i;
            do {
                h->xa_->add_imm(h->x_tmp_vec[count++], h->X_SP, i * vlen,
                        h->X_DEFAULT_ADDR);
                i++;
            } while (i < tail_vecs_to_preserve && count < h->x_tmp_vec_size);

            for (int j = 0; j < count; j++)
                h->xa_->st1w(ZRegS(preserved_vec_idxs[idx_off + ii++]),
                        p_512 / T_z, ptr(h->x_tmp_vec[j]));
        }

        if (idx_off) {
            h->xa_->sub_imm(h->X_SP, h->X_SP, idx_off * vlen, h->X_TMP_0);
        }
    }

    assign_regs();
    set_coef_to_regs();
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::injector_postamble() {
    using namespace Xbyak_aarch64::util;
    if (!save_state_) return;

    size_t i = 0;

    while (i < preserved_vecs_count) {
        int count = 0;
        int ii = i;
        do {
            h->xa_->add_imm(h->x_tmp_vec[count++], h->X_SP, i * vlen,
                    h->X_DEFAULT_ADDR);
            i++;
        } while (i < preserved_vecs_count && count < h->x_tmp_vec_size);

        for (int j = 0; j < count; j++)
            h->xa_->ld1w(ZRegS(preserved_vec_idxs[ii++]), p_512 / T_z,
                    ptr(h->x_tmp_vec[j]));
    }

    if (preserved_vecs_count)
        h->xa_->add_imm(
                h->X_SP, h->X_SP, preserved_vecs_count * vlen, h->X_TMP_0);

    for (int i = aux_gprs_count() - 1; i >= 0; --i)
        h->ldr(XReg(preserved_gpr_idxs[i]), post_ptr(h->X_SP, 8));
    h->ldr(x_table, post_ptr(h->X_SP, 8));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::assign_regs() {
    /* For translation of x64's memory operand instructions */
    z_tmp = TRegS(static_cast<uint32_t>(preserved_vec_idxs[0]));

    vmm_mask = TRegS(preserved_vec_idxs[1]);
    vmm_aux0 = TRegS(preserved_vec_idxs[1]);
    vmm_aux1 = TRegS(preserved_vec_idxs[2]);
    vmm_aux2 = TRegS(preserved_vec_idxs[3]);
    vmm_aux3 = TRegS(preserved_vec_idxs[4]);
    vmm_aux4 = TRegS(preserved_vec_idxs[5]);
    vmm_aux5 = TRegS(preserved_vec_idxs[6]);
    vmm_aux6 = TRegS(preserved_vec_idxs[7]);
    vmm_aux7 = TRegS(preserved_vec_idxs[8]);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::set_coef_to_regs() {
    using namespace alg_kind;

    if (is_fwd_) {
        switch (alg_) {
            case eltwise_relu_use_dst_for_bwd:
            case eltwise_relu:
                if (alpha_ != 0.f) table_val(alpha, z_tmp);
                break;
            case eltwise_elu_use_dst_for_bwd:
            case eltwise_elu: table_val(alpha, vmm_aux4); break;
            case eltwise_tanh_use_dst_for_bwd:
            case eltwise_tanh:
            case eltwise_square: break;
            case eltwise_abs: break;
            case eltwise_sqrt_use_dst_for_bwd:
            case eltwise_sqrt:
            case eltwise_swish: break;
            case eltwise_linear:
                table_val(alpha, z_tmp);
                table_val(beta, vmm_aux0);
                break;
            case eltwise_bounded_relu: table_val(alpha, z_tmp); break;
            case eltwise_soft_relu:
            case eltwise_logistic_use_dst_for_bwd:
            case eltwise_logistic:
            case eltwise_exp_use_dst_for_bwd:
            case eltwise_exp: break;
            case eltwise_gelu_tanh: break;
            case eltwise_log: break;
            case eltwise_clip:
                table_val(alpha, z_tmp);
                table_val(beta, vmm_aux0);
                break;
            case eltwise_pow: break;
            case eltwise_gelu_erf: break;
            case eltwise_round:
            default: assert(!"unsupported eltwise algorithm");
        }
    } else {
        switch (alg_) {
            case eltwise_relu_use_dst_for_bwd:
            case eltwise_relu: table_val(alpha, z_tmp); break;
            case eltwise_elu_use_dst_for_bwd:
            case eltwise_elu:
            case eltwise_tanh_use_dst_for_bwd:
            case eltwise_tanh:
            case eltwise_square:
            case eltwise_abs:
            case eltwise_sqrt_use_dst_for_bwd:
            case eltwise_sqrt:
            case eltwise_linear:
            case eltwise_bounded_relu:
            case eltwise_soft_relu:
            case eltwise_logistic_use_dst_for_bwd:
            case eltwise_logistic:
            case eltwise_exp_use_dst_for_bwd:
            case eltwise_exp: break;
            case eltwise_gelu_tanh: break;
            case eltwise_swish:
            case eltwise_log: break;
            case eltwise_clip:
                table_val(beta, z_tmp);
                table_val(alpha, vmm_aux0);
                break;
            case eltwise_pow:
            case eltwise_gelu_erf: break;
            default: assert(!"unsupported eltwise algorithm");
        }
    }
}
// Uses injector masks objects: k_mask (>= avx512_common) or vmm_mask (<= avx2).
// Stores a mask by applying cmpps on two inputs w/ a given predicate.
template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::compute_cmp_mask(
        const TRegS &vmm_src, const TRegS &compare_operand, int cmp_predicate) {

    enum {
        EQ_OQ = 0,
        LT_OS = 1,
        LE_OS = 2,
        UNORD_Q = 3,
        NEQ_UQ = 4,
        NLT_US = 5,
        NLE_US = 6,
        ORD_Q = 7,
        EQ_UQ = 8,
        NGE_US = 9,
        NGT_US = 10,
        FALSE_OQ = 11,
        NEQ_OQ = 12,
        GE_OS = 13,
        GT_OS = 14,
        TRUE_UQ = 15,
        EQ_OS = 16,
        LT_OQ = 17,
        LE_OQ = 18,
        UNORD_S = 19,
        NEQ_US = 20,
        NLT_UQ = 21,
        NLE_UQ = 22,
        ORD_S = 23,
        EQ_US = 24,
        NGE_UQ = 25,
        NGT_UQ = 26,
        FALSE_OS = 27,
        NEQ_OS = 28,
        GE_OQ = 29,
        GT_OQ = 30,
        TRUE_US = 31,
    };

    h->xa_->mov(PRegB(IDX(p_tmp0)), h->P_ALL_ONE / T_z, h->P_ALL_ONE.b);
    switch (cmp_predicate) {
        case EQ_OQ:
            h->fcmeq(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case LT_OS:
            h->fcmlt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case LE_OS:
            h->fcmle(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NEQ_UQ:
            h->fcmne(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NLT_US:
            h->fcmge(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NLE_US:
            h->fcmgt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case EQ_UQ:
            h->fcmeq(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NGE_US:
            h->fcmlt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NGT_US:
            h->fcmle(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NEQ_OQ:
            h->fcmne(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case GE_OS:
            h->fcmge(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case GT_OS:
            h->fcmgt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case EQ_OS:
            h->fcmeq(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case LT_OQ:
            h->fcmlt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case LE_OQ:
            h->fcmle(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NEQ_US:
            h->fcmne(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NLT_UQ:
            h->fcmge(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NLE_UQ:
            h->fcmgt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case EQ_US:
            h->fcmeq(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NGE_UQ:
            h->fcmlt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NGT_UQ:
            h->fcmle(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case NEQ_OS:
            h->fcmne(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case GE_OQ:
            h->fcmge(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;
        case GT_OQ:
            h->fcmgt(
                    PRegS(IDX(p_mask)), p_tmp0 / T_z, vmm_src, compare_operand);
            break;

        case UNORD_Q:
        case ORD_Q:
        case FALSE_OQ:
        case TRUE_UQ:
        case UNORD_S:
        case ORD_S:
        case FALSE_OS:
        case TRUE_US:
        default: assert(!"Unsupported compare mode"); break;
    }
}

// Uses injector masks objects: k_mask (>= avx512_common) or vmm_mask (<= avx2).
// Blends a result of second input into a first input w/ a stored mask.
template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::blend_with_mask(
        const TRegS &vmm_dst, const TRegS &src) {
    h->sel(vmm_dst, p_mask / T_m, src, vmm_dst);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::exp_compute_vector_fwd(
        const TRegS &vmm_src) {

    // exp(x) =
    // = exp(n * ln(2) + r) // divide x by ln(2) and get quot and rem
    // = 2^n * exp(r) // simplify the exp(n*ln(2)) expression

    // get mask of values lower than log(FLT_MIN) to zero them in the output
    compute_cmp_mask(vmm_src, table_val(exp_ln_flt_min_f, z_tmp), _cmp_lt_os);

    table_val(exp_ln_flt_max_f, z_tmp);
    h->fminnm(z_tmp, p_512, vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(z_tmp)));

    table_val(exp_ln_flt_min_f, z_tmp);
    h->fmaxnm(z_tmp, p_512, vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(z_tmp)));

    h->xa_->mov(ZRegD(IDX(vmm_aux1)), ZRegD(IDX(vmm_src)));

    // calculate exp(x)
    // fx = x * log2ef + 0.5
    h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(exp_log2ef, z_tmp))));
    h->xa_->fadd(vmm_src, p_512 / T_m, 0.5f);

    // tmp = floorf(fx)
    h->frintm(vmm_aux2, p_512 / T_m, vmm_src);

    // keep vmm_src = fx for further computations
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux2)));

    // x = x - fx * ln2
    h->fmls(vmm_aux1, p_512 / T_m, vmm_aux2,
            ZRegS(IDX(table_val(ln2f, z_tmp))));

    // We do not count 2^n here, because n can reach 128 and 2^128 is not
    // representable by fp32, so to get around this problem, instead of computing
    // 2^n * exp(r) will be counted 2*2^(n-1)*exp(r), because 2^127
    // and 2 are numbers representable in fp32.

    // compute 2^(n-1)
    h->xa_->fsub(vmm_src, p_512 / T_m, 1.f);
    h->frinti(vmm_aux2, p_512 / T_m, vmm_src);
    h->fcvtzs(vmm_aux2, p_512 / T_m, vmm_aux2);
    h->xa_->add(
            vmm_aux2, vmm_aux2, ZRegS(IDX(table_val(exponent_bias, z_tmp))));
    h->lsl(vmm_aux2, vmm_aux2, n_mantissa_bits); //TRegS(6) = 2^-fx

    // use vmm_src as tmp vmm_zero when applying mask
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)));
    // set zeroes at those points which were < log(FLT_MIN)
    blend_with_mask(vmm_aux2, vmm_src);

    // compute polynomial
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(exp_pol, z_tmp, 4))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 3))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 2))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 1))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 0))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));

    // y = y * 2^n
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux2);
    h->xa_->fmul(vmm_src, p_512 / T_m, 2.f);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::relu_compute_vector_fwd(
        const TRegS &vmm_src) {
    /* Negative values are multiplied by alpha.
     Positive values are not modified. */
    h->xa_->mov(ZRegD(vmm_aux0.getIdx()), ZRegD(vmm_src.getIdx()));
    h->fminnm(vmm_src, p_512, 0.f);
    h->fmaxnm(vmm_aux0, p_512, 0.f);
    /* alpha is set to z_tmp in set_coef_to_regs(). */
    h->xa_->fmul(vmm_src, vmm_src, z_tmp);
    h->xa_->fadd(vmm_src, vmm_src, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::relu_zero_ns_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->fmaxnm(vmm_src, p_512, 0.f);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::elu_compute_vector_fwd(
        const TRegS &vmm_src) {
    // IMPORTANT: we use vmm_aux3 for the mask as exp_compute does not use it.
    h->xa_->mov(ZRegD(vmm_aux3.getIdx()), ZRegD(vmm_src.getIdx()));

    // compute exponent
    exp_compute_vector_fwd(vmm_src);

    // alpha * (exp(x) - 1)
    h->xa_->fsub(vmm_src, p_512 / T_m, 1.f);
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux4);

    // combine with mask
    h->xa_->fcmgt(p_mask.s, p_512 / T_z, vmm_aux3, 0.f);
    h->xa_->mov(vmm_src, p_mask / T_m, vmm_aux3);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::tanh_compute_vector_fwd(
        const TRegS &vmm_src) {
    // we add a check as the avx2 code cannot be used for avx
    using namespace Xbyak_aarch64::util;
    const int tanh_n_polynomials = 32;

    // register mapping
    // TODO: put sign on stack and alias zmm_table2 with vmm_sign to save a reg ?
    TRegS vmm_dst = vmm_aux1, vmm_src_shift = vmm_aux1, vmm_coeff = vmm_aux1,
          vmm_pol = vmm_aux2, vmm_indices = vmm_aux3,
          vmm_src_original = vmm_aux4, vmm_sign = vmm_aux4;

    auto vpermt2ps_aarch64 =
            [&](const ZRegS &d, const ZRegS &s, const ZRegS &s2, const ZRegS &t,
                    const ZRegS &t2, const ZRegS &t3, const PReg &p) {
                h->ptrue(p.b);
                h->xa_->mov(t, 0x1f);
                h->xa_->and_(ZRegB(t.getIdx()), p, ZRegB(s.getIdx()));
                for (int i = 0; i < 16; i++) {
                    h->cmpeq(h->P_TMP_0.s, p, t, 0);
                    h->xa_->sub(t, 0x1);
                    h->dup(t2, d[i]);
                    h->xa_->mov(t3, h->P_TMP_0 / T_m, t2);
                }
                for (int i = 0; i < 16; i++) {
                    h->cmpeq(h->P_TMP_0.s, p, t, i);
                    h->dup(t2, s2[i]);
                    h->xa_->mov(t3, h->P_TMP_0 / T_m, t2);
                }
                h->xa_->mov(ZRegD(d.getIdx()), ZRegD(t3.getIdx()));
            };

    // We split the positive domain in 33 intervals:
    // a) [0; linear_ubound]: in this interval tanh(x) = x
    // b) [linear_ubound; 0x1.8p-12]: This interval spans part of a
    //    half binade
    // c) [0x1.8p-12; 0x1.0p-11], ..., [0x1.8p2; 0x1.0p3]:
    //    one interval for each half binade, there are 29 of those
    // d) [0x1.0p3; saturation_ubound]:
    //    This interval spans part of a half binade
    // e) [0x1.205966p3; saturation_ubound]: in this interval, tanh(x) = 1
    // For b-d, we need 31 polynomials and will do a table lookup for those.
    // To simplify the logic, we will also put a) in the table.

    // The polynomials are of degree 6, so we need to gather 7 coefficients.
    // - sse4.1: we do it the naive way using vextract/vinsert.
    //           Here we will extract the indices in gpr only once and
    //           reuse them as there are only 4 of them.
    // - avx2: we use vpermps and blend for each coefficient.
    //         This needs an extra vmm to store the mask
    // - avx512: because the table fits in 2 registers, we can use vpermi2d.
    auto coeffs_address = [&](int coeff_off, int off = 0) {
        return table_val(
                tanh_pol_table, z_tmp, coeff_off * tanh_n_polynomials + off);
    };
    auto gather_coefficient_init = [&](TRegS vmm_pol_idx, int nelems) {
        switch (isa) {
            case sve_512: break;
            default: assert(!"unimplemented");
        }
    };
    auto gather_coefficient = [&](TRegS vmm_coeff, int coeff_idx,
                                      TRegS vmm_pol_idx) {
        switch (isa) {
                // use gather instruction
            case sve_512:
                // we use vpermt2ps to not override the indices
                // this also enables to save a register for table loading
                {
                    ZReg zmm_coeff(vmm_coeff.getIdx());
                    ZReg zmm_pol_idx(vmm_pol_idx.getIdx());
                    h->xa_->mov(ZRegD(IDX(zmm_coeff)),
                            ZRegD(IDX(coeffs_address(coeff_idx, 0))));

                    vpermt2ps_aarch64(ZRegS(IDX(zmm_coeff)),
                            ZRegS(IDX(zmm_pol_idx)),
                            ZRegS(IDX(coeffs_address(coeff_idx, 16))), vmm_aux5,
                            vmm_aux6, vmm_aux7, p_tmp0);
                    break;
                }
            default: assert(!"unimplemented");
        }
    };

    // because tanh(x) = -tanh(-x), we extract sign to make x postive
    // and reapply sign at the end
    h->xa_->mov(ZRegD(IDX(vmm_src_original)), ZRegD(IDX(vmm_src)));
    h->xa_->and_(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(positive_mask, z_tmp))));

    // We compute the indices for the table lookup
    h->xa_->mov(ZRegD(IDX(vmm_indices)), ZRegD(IDX(vmm_src)));
    h->xa_->sub(ZRegS(IDX(vmm_indices)), ZRegS(IDX(vmm_indices)),
            ZRegS(IDX(table_val(tanh_idx_bias, z_tmp))));
    h->xa_->and_(ZRegD(IDX(vmm_indices)), ZRegD(IDX(vmm_indices)),
            ZRegD(IDX(table_val(tanh_idx_mask, z_tmp))));
    h->lsr(ZRegS(IDX(vmm_indices)), ZRegS(IDX(vmm_indices)), 22);

    // we do the argument reduction
    h->xa_->mov(ZRegD(IDX(vmm_src_shift)), ZRegD(IDX(vmm_src)));
    h->xa_->and_(ZRegD(IDX(vmm_src_shift)), ZRegD(IDX(vmm_src_shift)),
            ZRegD(IDX(table_val(tanh_idx_mask, z_tmp))));
    h->xa_->fsub(vmm_src, vmm_src, ZRegS(IDX(vmm_src_shift)));

    // we gather and evaluate the polynonials
    gather_coefficient_init(vmm_indices, vlen / sizeof(float));
    gather_coefficient(vmm_pol, 6, vmm_indices);

    for (int deg = 5; deg >= 0; --deg) {
        gather_coefficient(vmm_coeff, deg, vmm_indices);
        h->fmad(ZRegS(IDX(vmm_pol)), p_512 / T_m, vmm_src,
                ZRegS(IDX(vmm_coeff)));
    }

    // we restore src with cleared sign, and keep sign
    assert(vmm_sign.getIdx() == vmm_src_original.getIdx());
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src_original)));
    h->xa_->and_(ZRegD(IDX(vmm_sign)), ZRegD(IDX(vmm_sign)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));
    h->xa_->and_(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(positive_mask, z_tmp))));

    // Now we blend the results
    // [saturation_ubound; +inf[ : we return +/- 1
    h->xa_->mov(ZRegD(IDX(vmm_dst)), ZRegD(IDX(table_val(one, z_tmp))));

    // [linear_ubound; saturation_lbound] : we return +/- P(x)
    h->xa_->mov(ZRegD(IDX(vmm_mask)),
            ZRegD(IDX(table_val(tanh_saturation_lbound, z_tmp))));

    compute_cmp_mask(vmm_mask, vmm_src, _cmp_gt_os);
    blend_with_mask(vmm_dst, vmm_pol);

    // [0; linear_ubound]  : we return x
    h->xa_->mov(ZRegD(IDX(vmm_mask)),
            ZRegD(IDX(table_val(tanh_linear_ubound, z_tmp))));

    compute_cmp_mask(vmm_mask, vmm_src, _cmp_gt_os);
    blend_with_mask(vmm_dst, vmm_src);

    // We reapply the sign and return
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_dst)), ZRegD(IDX(vmm_sign)));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::gelu_tanh_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(vmm_src)));

    // compute G(x) = sqrt_root_two_over_pi * x * (1 + fitting_const * x * x)
    h->xa_->fmul(vmm_src, vmm_src, vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_aux1)),
            ZRegD(IDX(table_val(gelu_tanh_fitting_const, z_tmp))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux0);
    h->xa_->fmul(vmm_src, vmm_src,
            ZRegS(IDX(table_val(gelu_tanh_sqrt_two_over_pi, z_tmp))));

    // save x on stack as tanh uses vmm_aux0
    h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->str(ZReg(IDX(vmm_aux0)), ptr(h->X_TMP_0));

    // compute tanh(G(x))
    tanh_compute_vector_fwd(vmm_src);

    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux0)), ptr(h->X_TMP_0));
    h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    // compute 0.5 * x * (1 + tanh(G(x)))
    h->xa_->fadd(vmm_src, p_512 / T_m, 1.f);
    h->xa_->fmul(vmm_src, p_512 / T_m, 0.5f);
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::square_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->xa_->fmul(vmm_src, vmm_src, vmm_src);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::abs_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->xa_->fabs(vmm_src, p_512 / T_m, vmm_src);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::sqrt_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->xa_->fsqrt(vmm_src, p_512 / T_m, vmm_src);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::linear_compute_vector_fwd(
        const TRegS &vmm_src) {
    // compute x = alpha * x + beta;
    h->fmad(vmm_src, p_512 / T_m, z_tmp, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::bounded_relu_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->fmaxnm(vmm_src, p_512, 0.f);
    h->fminnm(vmm_src, p_512, z_tmp);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::clip_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->fmaxnm(vmm_src, p_512, z_tmp);
    h->fminnm(vmm_src, p_512, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::soft_relu_compute_vector_fwd(
        const TRegS &vmm_src) {
    // ln(1 + exp(x)) =
    // = ln(1 + exp(n * ln(2) + r)) // divide x by ln(2) and get quot and rem
    // = ln(1 + 2^n * exp(r)) // simplify the exp(n*ln(2)) expression
    // = ln(2 ^ 0 + 2^n * exp(r)) // note 1 = 2^0
    // = ln(2 ^ (n - n) + 2^n * exp(r)) // 2^0 = 2^(n-n)
    // = ln(2 ^ n * (2^-n + exp(r))) // factorize with 2^n
    // = n * ln(2) + ln(2^-n + exp(r)) // take the 2^n factor out of the ln

    // keep src for further computations
    h->xa_->mov(ZRegD(IDX(vmm_aux2)), ZRegD(IDX(vmm_src)));

    h->fminnm(ZRegS(IDX(table_val(exp_ln_flt_max_f, z_tmp))), p_512, vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(z_tmp)));
    h->fmaxnm(ZRegS(IDX(table_val(exp_ln_flt_min_f, z_tmp))), p_512, vmm_src);

    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(z_tmp)));
    h->xa_->mov(ZRegD(IDX(vmm_aux1)), ZRegD(IDX(vmm_src)));

    // calculate exp(x)
    // fx = x * log2ef + 0.5
    h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(exp_log2ef, z_tmp))));
    h->xa_->fadd(vmm_src, p_512 / T_m, 0.5f);

    // tmp = floorf(fx)
    h->frintm(vmm_aux0, p_512 / T_m, vmm_src);

    // keep vmm_src = fx for further computations
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux0)));

    // x = x - fx * ln2
    h->xa_->fmul(vmm_aux0, vmm_aux0, ZRegS(IDX(table_val(ln2f, z_tmp))));
    h->xa_->fsub(vmm_aux1, vmm_aux1, vmm_aux0);
    // compute exponent polynomial
    h->xa_->mov(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(table_val(exp_pol, z_tmp, 4))));
    h->fmad(vmm_aux3, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 3))));
    h->fmad(vmm_aux3, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 2))));
    h->fmad(vmm_aux3, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 1))));
    h->fmad(vmm_aux3, p_512 / T_m, vmm_aux1,
            ZRegS(IDX(table_val(exp_pol, z_tmp, 0))));
    h->fmad(vmm_aux3, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));

    // We do not count 2^-n here, because n can reach 128 and 2^(-128) is not
    // representable by fp32, so to get around this problem, instead of computing
    // 2^-n + exp(r) will be counted (2^-(n-1) + 2*exp(r))/2, because 2^(-127)
    // and 2 are numbers representable in fp32.

    // compute 2^-(n-1)
    // vmm_src now represents n-1
    h->xa_->fsub(vmm_src, p_512 / T_m, 1.f);
    h->xa_->fneg(vmm_aux1, p_512 / T_m, vmm_src);

    h->frinti(vmm_aux1, p_512 / T_m, vmm_aux1);
    h->fcvtzs(vmm_aux1, p_512 / T_m, vmm_aux1);
    // restore vmm_src to n
    h->xa_->fadd(vmm_src, p_512 / T_m, 1.f);

    h->xa_->add(
            vmm_aux1, vmm_aux1, ZRegS(IDX(table_val(exponent_bias, z_tmp))));
    h->lsl(vmm_aux1, vmm_aux1, n_mantissa_bits);
    // calculate ln(1 + y)
    h->xa_->fmul(vmm_aux3, p_512 / T_m, 2.f); // 2*exp(r)
    h->xa_->fadd(vmm_aux3, vmm_aux3,
            vmm_aux1); // 2^-(n-1) + 2*exp(r)
    h->xa_->fdiv(vmm_aux3, p_512 / T_m,
            ZRegS(IDX(table_val(two, z_tmp)))); // (2^-(n-1) + 2*exp(r))/2

    // frexp()
    h->lsr(vmm_src, vmm_aux3, n_mantissa_bits);
    h->scvtf(vmm_src, p_512 / T_m, vmm_src);
    // got n. where n is x = 2^n * y. y = 0.5 .. 1
    h->xa_->fsub(vmm_src, vmm_src,
            ZRegS(IDX(table_val(soft_relu_one_twenty_six, z_tmp))));

    // and with mask (to get 0.5 * mantissa)
    h->xa_->and_(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_aux3)),
            ZRegD(IDX(table_val(soft_relu_mantissa_sign_mask, z_tmp))));
    // got y. (mantisa)  0.5 < y < 1 (or with (to get 0.5 * mantissa))
    h->orr(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_aux3)),
            ZRegD(IDX(table_val(half, z_tmp))));
    // y  = y - 1
    h->xa_->fsub(vmm_aux3, p_512 / T_m, 1.f);

    // compute log1p polynomial
    h->xa_->mov(ZRegD(IDX(vmm_aux1)),
            ZRegD(IDX(table_val(soft_relu_pol, z_tmp, 8))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 7))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 6))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 5))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 4))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 3))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 2))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 1))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(soft_relu_pol, z_tmp, 0))));
    //calculate ln(2) * n
    h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(ln2f, z_tmp))));
    h->xa_->fadd(vmm_src, vmm_src, vmm_aux1);
    h->xa_->fadd(vmm_src, vmm_src, vmm_aux0);

    // get vmm_mask = src > max logf
    // y = (x < max log f) ? soft_relu(x) : x
    compute_cmp_mask(vmm_aux2, table_val(exp_ln_flt_max_f, z_tmp), _cmp_gt_os);
    blend_with_mask(vmm_src, vmm_aux2);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::logistic_compute_vector_fwd(
        const TRegS &vmm_src) {
    // To avoid exp(x) overflow happened at x > logf(FLT_MAX), negate positive,
    // compute exp(x), where x <= 0 to get 0 <= exp(x) <= 1 and restore value
    // sign at the end. This is possible due to logistic is symmetric function.
    // IMPORTANT: we use vmm_aux3 for the mask as exp_compute does not use it.
    h->xa_->mov(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_src)));
    // we store the original sign and make x negative
    h->xa_->and_(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_aux3)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));
    h->orr(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));

    exp_compute_vector_fwd(vmm_src);

    // dup exp(x)
    h->xa_->mov(ZRegD(IDX(vmm_aux1)), ZRegD(IDX(vmm_src)));
    // (exp(x) + 1)
    h->xa_->fadd(vmm_aux1, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));
    // y = exp(x) / (exp(x) + 1)
    h->xa_->fdiv(vmm_src, p_512, vmm_aux1);

    // Now we have to apply the "symmetry" based on original sign
    h->xa_->mov(ZRegD(IDX(vmm_aux2)), ZRegD(IDX(table_val(one, z_tmp))));
    h->xa_->fsub(vmm_aux2, vmm_aux2, vmm_src);

    h->xa_->and_(ZRegD(IDX(z_tmp)), ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_aux3)));
    h->cmpne(PRegS(IDX(p_mask)), p_512 / T_z, z_tmp, 0);

    blend_with_mask(vmm_aux2, vmm_src);

    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux2)));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::swish_compute_vector_fwd(
        const TRegS &vmm_src) {
    // Save src data on stack for later usage
    h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));
    // x*alpha
    h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    // sigmoid(x*alpha)
    logistic_compute_vector_fwd(vmm_src);
    // x*sigmoid(alpha*x)
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux0)), ptr(h->X_TMP_0));
    h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    h->xa_->fmul(vmm_src, vmm_src, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::log_compute_vector_fwd(
        const TRegS &vmm_src) {
    // From J.-M. Muller and others, Handbook of Floating-Point Arithmetic, 2010
    // Here is a brief mathematics to approximate log(x):
    // log(x) = E * log(2) + log(y), where -log(2)/2 <= log(y) <= log(2)/2;
    // log(y) = log(1 + z) - log(r_i), where z = y * r_i - 1, r_i approximates
    //   1 / y, i is index of one of precomputed values;
    // log(1 + z) ~~ polynomial(z), =>
    // if (x is normal)
    //     log(x) ~~ E * log(2) + polynomial(z) - log(r_i),
    // where log(r_i) is table value.
    //
    // If (x == 0) result = -inf;
    // If (x < 0) result = qnan;

    // save source on stack to check neg and zero values at the end
    h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));

    // compute i
    const int approx_order = 5;
    h->lsr(vmm_aux1, vmm_src, n_mantissa_bits - approx_order);
    h->xa_->and_(ZRegD(IDX(vmm_aux1)), ZRegD(IDX(vmm_aux1)),
            ZRegD(IDX(table_val(log_five_bit_offset, z_tmp))));
    h->lsl(vmm_aux1, vmm_aux1,
            1); // multiply i by 2

    // compute anticancellation i
    h->lsr(vmm_aux2, vmm_aux1, approx_order);

    // get E, don't care about sign as only positive numbers are considered
    h->lsr(vmm_aux3, vmm_src, n_mantissa_bits);
    h->xa_->add(vmm_aux3, vmm_aux3, vmm_aux2);
    h->scvtf(vmm_aux3, p_512 / T_m, vmm_aux3);

    // get m (mantissa)
    h->eor(ZRegD(IDX(vmm_aux2)), ZRegD(IDX(vmm_aux2)),
            ZRegD(IDX(table_val(exponent_bias, z_tmp))));
    h->lsl(vmm_aux2, vmm_aux2, n_mantissa_bits);
    h->xa_->and_(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(log_mantissa_mask, z_tmp))));
    h->orr(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux2)));

    // At first, adjust indices for table structure which broadcasts elements
    h->lsl(vmm_aux1, vmm_aux1,
            4); // multiply by simd_w = 16

    const auto it = entry_map_.find(log_predefined_vals);
    assert(it != entry_map_.end());
    const auto table_start_idx = (*it).second.off;

    auto gather_table_values = [&](const TRegS &vmm_dst, const TRegS &vmm_idxs,
                                       size_t offt = 0) {
        h->ptrue(PRegS(IDX(p_mask)), VL16);
        h->add_imm(
                h->X_DEFAULT_ADDR, x_table, table_start_idx + offt, h->X_TMP_1);

        h->xa_->mov(ZRegD(IDX(z_tmp)), ZRegD(IDX(vmm_idxs)));
        h->xa_->mul(z_tmp, 4);

        h->ld1w(z_tmp, p_mask / T_z, ptr(h->X_DEFAULT_ADDR, z_tmp, SXTW));
        h->xa_->mov(vmm_dst, p_mask / T_m, z_tmp);
        h->pfalse(PRegB(IDX(p_mask)));
    };

    // get r_i, same as table(i)
    gather_table_values(vmm_aux2, vmm_aux1, 0);

    // compute relative error (rel_err = m * r_i - 1)
    /* [info]Expand from the content of the process, not from the instruction. */
    h->xa_->fmul(vmm_aux2, vmm_aux2, vmm_src);
    h->xa_->fsub(vmm_aux2, p_512 / T_m, 1.f);

    // compute polynomial(rel_err)
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(log_pol, z_tmp, 3))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux2,
            ZRegS(IDX(table_val(log_pol, z_tmp, 2))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux2,
            ZRegS(IDX(table_val(log_pol, z_tmp, 1))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux2,
            ZRegS(IDX(table_val(log_pol, z_tmp, 0))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux2, ZRegS(IDX(table_val(one, z_tmp))));
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux2);

    // get log(r_i) = table(i+1)
    gather_table_values(vmm_aux2, vmm_aux1, vlen);

    // compute partial result (pres = E * ln(2) - log(r_i))
    h->fmla(vmm_aux2, p_512 / T_m, vmm_aux3,
            ZRegS(IDX(table_val(ln2f, z_tmp))));

    // compute (result = polynomial + pres) w/ TwoSum algorithm
    // TODO: restore this instead of version below when asserts are gone
    // h->uni_vaddps(vmm_aux1, vmm_src, vmm_aux2); // res_hi = pol + pres
    // h->uni_vsubps(vmm_aux3, vmm_aux1, vmm_aux2); // res_lo = res_hi - pres
    // h->uni_vsubps(vmm_aux3, vmm_aux3, vmm_src); // res_lo = res_lo - pol
    // h->uni_vaddps(vmm_src, vmm_aux1, vmm_aux3); // res_hi = pol + pres

    h->xa_->mov(ZRegD(IDX(vmm_aux1)), ZRegD(IDX(vmm_src)));
    h->xa_->fadd(vmm_aux1, vmm_aux1, vmm_aux2);
    h->xa_->mov(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_aux1)));
    h->xa_->fsub(vmm_aux3, vmm_aux3,
            vmm_aux2); // res_lo = res_hi - pres
    h->xa_->fsub(vmm_aux3, vmm_aux3,
            vmm_src); // res_lo = res_lo - pol
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux1)));
    h->xa_->fadd(vmm_src, vmm_src, vmm_aux3);

    // Check original source for zero and neg values. skip blend w/ extreme
    // values if all src values were positive.
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux1)), ptr(h->X_TMP_0));
    h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    Label end_log_label;
    compute_cmp_mask(vmm_aux1, table_val(zero, z_tmp), _cmp_le_os);

    h->orrs(h->P_TMP_0.b, h->P_ALL_ONE / Xbyak_aarch64::T_z,
            Xbyak_aarch64::PRegB(p_mask.getIdx()),
            Xbyak_aarch64::PRegB(p_mask.getIdx()));

    h->b(EQ, end_log_label);

    // Blend extreme values into src if reach here.
    // First zero for -inf values...
    compute_cmp_mask(vmm_aux1, table_val(zero, z_tmp), _cmp_eq_oq);
    blend_with_mask(vmm_src, table_val(log_minus_inf, z_tmp));

    // ...then negative for qnan values.
    compute_cmp_mask(vmm_aux1, table_val(zero, z_tmp), _cmp_lt_os);
    blend_with_mask(vmm_src, table_val(log_qnan, z_tmp));

    h->L(end_log_label);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::pow_compute_vector_fwd(
        const TRegS &vmm_src) {
    // dispatch between special cases.
    if (beta_ == -1) { // alpha / x
        h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(table_val(alpha, z_tmp))));

        h->xa_->mov(ZRegD(IDX(z_tmp)), ZRegD(IDX(vmm_src)));
        h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux0)));
        h->xa_->fdiv(vmm_src, p_512 / T_m, z_tmp);
    } else if (beta_ == 0) { // alpha
        h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(alpha, z_tmp))));
    } else if (beta_ == 0.5) { // alpha * sqrt(x)
        sqrt_compute_vector_fwd(vmm_src);
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    } else if (beta_ == 1) { // alpha * x
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    } else if (beta_ == 2) { // alpha * x^2
        square_compute_vector_fwd(vmm_src);
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    } else { // general path
        // caller obligation to save gprs as callee may use them
        size_t gpr_size = 5;
        Xbyak_aarch64::XReg gprs_to_save[]
                = {h->x8, h->x9, h->x10, h->x11, h->x0};

        size_t n_gprs_to_save = sizeof(gprs_to_save) / sizeof(gprs_to_save[0]);

        h->sub_imm(h->X_SP, h->X_SP, n_gprs_to_save * gpr_size, h->X_TMP_0);
        for (size_t i = 0; i < n_gprs_to_save; ++i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * gpr_size, h->X_TMP_1);
            h->str(XReg(IDX(gprs_to_save[i])), ptr(h->X_TMP_0));
        }

        // caller obligation to save k-regs as callee may use them
        size_t n_k_regs_to_save = 8;
        h->sub_imm(
                h->X_SP, h->X_SP, n_k_regs_to_save * k_mask_size, h->X_TMP_0);
        for (size_t i = 0; i < n_k_regs_to_save; ++i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * k_mask_size, h->X_TMP_1);
            h->str(PReg(static_cast<uint32_t>(i)), ptr(h->X_TMP_0));
        }

        // 1. Caller obligation to save vector registers as callee may use them.
        // 2. Additionally save space for vmm_src, to put the answer in-place on
        // this space and space for beta.
        // 3. There is an implicit assumption that the host code uses the same
        // `isa` as the injector. Once the assumption is wrong, `vecs_count` and
        // `vlen` should be replaced with `host_isa::vlen` and
        // `host_isa::vecs_count`.
        h->sub_imm(h->X_SP, h->X_SP, (vecs_count + 2) * vlen, h->X_TMP_0);

        for (size_t i = 2; i < vecs_count + 2; ++i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * vlen, h->X_TMP_1);
            h->str(ZReg(IDX(TRegS(i - 2))), ptr(h->X_TMP_0));
        }

        h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
        h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));

        h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(beta, z_tmp))));

        h->add_imm(h->X_TMP_0, h->X_SP, vlen, h->X_TMP_1);
        h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));

        // save function address in gpr to pass in in call instruction
        h->xa_->mov_imm(h->x0, reinterpret_cast<uintptr_t>(powf));

        // align stack on 16-byte as ABI requires
        h->xa_->mov(h->x0, h->X_SP);

        uint64_t mask = ~uint64_t(0xffffffff);
        unsigned bits = (mask & 0xf) ? 64 : 32;
        h->xa_->mov_imm(h->X_TMP_0, int64_t(bits));
        h->xa_->and_(h->x0, h->x0, h->X_TMP_0);

        h->xa_->sub(h->X_SP, h->X_SP, h->x0);

        // Take src, apply powf on it and replace value on a stack with dst.
        VReg xmm0 = VReg(0), xmm1 = VReg(1);
        for (size_t i = 0; i < vlen / sizeof(float); ++i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * sizeof(float), h->X_TMP_1);
            h->xa_->add(h->X_TMP_0, h->X_TMP_0, h->x0);
            h->ld1(VReg(IDX(xmm0)).s[0], ptr(h->X_TMP_0));
            h->xa_->mov(z_tmp, 0);
            for (int ii = 1; ii < 4; ii++) {
                h->xa_->mov(VReg(IDX(xmm0)).s[ii], VReg(IDX(z_tmp)).s[0]);
            }
            // beta
            h->add_imm(h->X_TMP_0, h->X_SP, vlen, h->X_TMP_1);
            h->xa_->add(h->X_TMP_0, h->X_TMP_0, h->x0);
            h->ld1(VReg(IDX(xmm1)).s[0], ptr(h->X_TMP_0));
            h->xa_->mov(z_tmp, 0);
            for (int ii = 1; ii < 4; ii++) {
                h->xa_->mov(VReg(IDX(xmm1)).s[ii], VReg(IDX(z_tmp)).s[0]);
            }

            h->br(h->x0);

            h->add_imm(h->X_TMP_0, h->X_SP, i * sizeof(float), h->X_TMP_1);
            h->xa_->add(h->X_TMP_0, h->X_TMP_0, h->x0);
            h->xa_->st1(VReg(IDX(xmm0)).s[0], ptr(h->X_TMP_0));
        }

        h->xa_->add(h->X_SP, h->X_SP, h->x0);

        // restore vector registers
        for (size_t i = vecs_count + 1; i >= 2; --i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * vlen, h->X_TMP_1);
            h->ldr(ZReg(IDX(TRegS(i - 2))), ptr(h->X_TMP_0));
        }

        h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
        h->ldr(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));

        h->add_imm(h->X_SP, h->X_SP, (vecs_count + 2) * vlen, h->X_TMP_0);
        // restore k registers

        for (int i = n_k_regs_to_save - 1; i >= 0; --i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * k_mask_size, h->X_TMP_1);
            h->ldr(PReg(static_cast<uint32_t>(i)), ptr(h->X_TMP_0));
        }
        h->add_imm(
                h->X_SP, h->X_SP, n_k_regs_to_save * k_mask_size, h->X_TMP_0);

        // restore gpr registers
        for (int i = n_gprs_to_save - 1; i >= 0; --i) {
            h->add_imm(h->X_TMP_0, h->X_SP, i * gpr_size, h->X_TMP_1);
            h->ldr(gprs_to_save[i], ptr(h->X_TMP_0));
        }
        h->add_imm(h->X_SP, h->X_SP, n_gprs_to_save * gpr_size, h->X_TMP_0);
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    }
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::gelu_erf_compute_vector_fwd(
        const TRegS &vmm_src) {
    // Here we approximate erf(x) using the expression by
    // Abramowitz and Stegun from ``Handbook of Mathematical
    // Functions''
    // NOTE: The performance of this kernel can be further improved
    // with a minimax polynomialial expansion, thereby avoiding division
    // and exp. However, so far, this has costed larger accuracy
    // differences with respect to glibc erf based GELU, in particular
    // ~1.0e-5 -- 1.0e-3 absolute error at s = -5.

    // x = s / sqrt(2)
    h->xa_->fmul(vmm_src, vmm_src,
            ZRegS(IDX(table_val(gelu_erf_one_over_sqrt_two, z_tmp))));

    // IMPORTANT: we use vmm_aux3 to save `x` as exp_compute does not use it.
    h->xa_->mov(ZRegD(IDX(vmm_aux3)), ZRegD(IDX(vmm_src)));

    // -exp(-x*x)
    h->xa_->fmul(vmm_src, vmm_src, vmm_src);
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));

    exp_compute_vector_fwd(vmm_src);
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));

    // get sign
    h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(vmm_aux3)));
    h->xa_->and_(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(vmm_aux0)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));

    // abs(x)
    h->xa_->mov(ZRegD(IDX(vmm_aux1)), ZRegD(IDX(vmm_aux3)));
    abs_compute_vector_fwd(vmm_aux1);

    // t = 1 / (p*x + 1)
    h->xa_->mov(ZRegD(IDX(vmm_aux2)),
            ZRegD(IDX(table_val(gelu_erf_approx_const, z_tmp))));
    h->fmad(vmm_aux2, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));

    h->xa_->mov(ZRegD(IDX(vmm_aux4)), ZRegD(IDX(table_val(one, z_tmp))));
    h->xa_->fdiv(vmm_aux4, p_512, vmm_aux2);

    // -exp(-x*x)*t
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux4);

    // compute polynomialial r
    h->xa_->mov(ZRegD(IDX(vmm_aux1)),
            ZRegD(IDX(table_val(gelu_erf_pol, z_tmp, 4))));

    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 3))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 2))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 1))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 0))));

    // erf = sign * (1 - r * t * exp(-x*x))
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux0)));

    // S = 0.5 * s = x / sqrt^2(2)
    h->xa_->fmul(vmm_aux3, vmm_aux3,
            ZRegS(IDX(table_val(gelu_erf_one_over_sqrt_two, z_tmp))));
    // GELU = 0.5 * s * (1 + erf) = S + S * erf
    h->fmad(vmm_src, p_512 / T_m, vmm_aux3, vmm_aux3);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::relu_compute_vector_bwd(
        const TRegS &vmm_src) {
    h->fcmgt(p_mask.s, p_512 / T_z, vmm_src, 0.f);
    h->xa_->mov(ZRegD(vmm_src.getIdx()), ZRegD(z_tmp.getIdx()));
    h->xa_->fmov(vmm_src, p_mask / T_m, 1.f);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::elu_compute_vector_bwd(
        const TRegS &vmm_src) {
    if (!use_dst_) {
        // R = exp(s)
        exp_compute_vector_fwd(vmm_src);
        // after exponentiation, get mask by comparing with exp(0)=1.f, not 0.f
        compute_cmp_mask(vmm_src, table_val(one, z_tmp), _cmp_gt_os);
        // R * alpha, then blend with 1.f
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    } else {
        // get mask of `d` > 0
        compute_cmp_mask(vmm_src, table_val(zero, z_tmp), _cmp_gt_os);
        // R = `d` + alpha, then blend with 1.f
        h->xa_->fadd(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    }
    blend_with_mask(vmm_src, table_val(one, z_tmp));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::tanh_compute_vector_bwd(
        const TRegS &vmm_src) {
    // res = 1 - d^2 = 1 - tanh^2(s)
    if (!use_dst_) tanh_compute_vector_fwd(vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(table_val(one, z_tmp))));

    h->fmls(vmm_aux0, p_512 / T_m, vmm_src, vmm_src);

    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux0)));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::gelu_tanh_compute_vector_bwd(
        const TRegS &vmm_src) {
    h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(vmm_src)));

    // compute G1(x) = sqrt_root_two_over_pi * x * (1 + fitting_const * x^2)
    // compute G2(x) = sqrt_root_two_over_pi * x * (1 + 3 * fitting_const * x^2)
    h->xa_->fmul(vmm_src, vmm_src, vmm_src);

    // keep G2 in a separate register
    h->xa_->mov(ZRegD(IDX(vmm_aux2)),
            ZRegD(IDX(table_val(gelu_tanh_fitting_const_times_three, z_tmp))));
    h->fmad(vmm_aux2, p_512 / T_m, vmm_src, ZRegS(IDX(table_val(one, z_tmp))));

    h->xa_->mov(ZRegD(IDX(vmm_aux1)),
            ZRegD(IDX(table_val(gelu_tanh_fitting_const, z_tmp))));
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));
    h->xa_->fmul(vmm_aux0, vmm_aux0,
            ZRegS(IDX(table_val(gelu_tanh_sqrt_two_over_pi, z_tmp))));
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux0);
    h->xa_->fmul(vmm_aux2, vmm_aux2, vmm_aux0);

    // save G2 on stack as tanh uses all available registers
    h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->str(ZReg(IDX(vmm_aux2)), ptr(h->X_TMP_0));

    // T = tanh(G1(x))
    tanh_compute_vector_fwd(vmm_src);

    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux2)), ptr(h->X_TMP_0));
    h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    // compute 0.5 * (1 + T) * (1 + G2 * (1 - T))
    // 1) R = G2 * (1 - T) = G2 - G2 * T
    h->fmls(vmm_aux2, p_512 / T_m, vmm_aux2, vmm_src);
    // 2) Q = 1 + T
    h->xa_->fadd(vmm_src, vmm_src, ZRegS(IDX(table_val(one, z_tmp))));
    // 3) res = Q * (1 + R) = Q + Q * R
    h->fmla(vmm_src, p_512 / T_m, vmm_src, vmm_aux2);

    h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(half, z_tmp))));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::square_compute_vector_bwd(
        const TRegS &vmm_src) {
    // res = 2 * s
    h->xa_->fmul(vmm_src, p_512 / T_m, 2.f);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::abs_compute_vector_bwd(
        const TRegS &vmm_src) {
    // replace positive values with 1.f
    compute_cmp_mask(vmm_src, table_val(zero, z_tmp), _cmp_gt_os);
    blend_with_mask(vmm_src, table_val(one, z_tmp));
    // replace negative values with -1.f
    compute_cmp_mask(vmm_src, table_val(zero, z_tmp), _cmp_lt_os);
    blend_with_mask(vmm_src, table_val(minus_one, z_tmp));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::sqrt_compute_vector_bwd(
        const TRegS &vmm_src) {
    // res = 0.5 / d = 0.5 / sqrt(s)
    if (!use_dst_) sqrt_compute_vector_fwd(vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(table_val(half, z_tmp))));
    h->xa_->fdiv(vmm_aux0, p_512, vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux0)));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::linear_compute_vector_bwd(
        const TRegS &vmm_src) {
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(alpha, z_tmp))));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::bounded_relu_compute_vector_bwd(
        const TRegS &vmm_src) {
    // get mask of values > alpha and blend with 0.f
    compute_cmp_mask(vmm_src, table_val(alpha, z_tmp), _cmp_gt_os);
    blend_with_mask(vmm_src, table_val(zero, z_tmp));
    // make all negative values zeros
    h->xa_->fmov(z_tmp, 0.f);
    h->fmaxnm(vmm_src, p_512, z_tmp);

    // everything bigger than 0.f should be 1.f
    compute_cmp_mask(vmm_src, table_val(zero, z_tmp), _cmp_gt_os);
    blend_with_mask(vmm_src, table_val(one, z_tmp));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::soft_relu_compute_vector_bwd(
        const TRegS &vmm_src) {
    logistic_compute_vector_fwd(vmm_src);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::logistic_compute_vector_bwd(
        const TRegS &vmm_src) {
    // res = d * (1 - d) = d - d * d; d = logistic(s)
    if (!use_dst_) logistic_compute_vector_fwd(vmm_src);
    // h->uni_vfnmadd231ps(vmm_src, vmm_src, vmm_src); // bless sse41
    h->xa_->mov(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(table_val(one, z_tmp))));

    h->xa_->fsub(vmm_aux0, vmm_aux0, vmm_src);

    h->xa_->fmul(vmm_src, vmm_src, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::exp_compute_vector_bwd(
        const TRegS &vmm_src) {
    if (!use_dst_) exp_compute_vector_fwd(vmm_src);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::swish_compute_vector_bwd(
        const TRegS &vmm_src) {
    // R = alpha * s
    h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));

    // Save R on stack for later usage
    h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));

    // Q = sigmoid(alpha * s)
    logistic_compute_vector_fwd(vmm_src);

    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux0)), ptr(h->X_TMP_0));

    h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    // compute Q * (1 + R * (1 - Q))
    // T = R * (1 - Q) = R - R * Q
    h->fmls(vmm_aux0, p_512 / T_m, vmm_aux0, vmm_src);

    // Q * (1 + T) = Q + Q * T
    h->fmla(vmm_src, p_512 / T_m, vmm_src, vmm_aux0);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::log_compute_vector_bwd(
        const TRegS &vmm_src) {
    // res = 1 / s
    /* Do not use 1.f, which is a float constant,
       but 1., which is a double constant. */
    h->xa_->fmov(z_tmp, 1.);
    h->xa_->fdiv(z_tmp, p_512, vmm_src);
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(z_tmp)));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::clip_compute_vector_bwd(
        const TRegS &vmm_src) {
    // set result with 1.
    /* Do not use 1.f, which is a float constant,
       but 1., which is a double constant. */
    h->xa_->fmov(vmm_aux1, 1.);

    // get mask of values > beta and blend with 0.f
    h->xa_->fcmgt(p_mask.s, p_512 / T_z, vmm_src, z_tmp);
    h->xa_->mov(vmm_aux1, p_mask / T_m, 0);
    // get mask of values <= alpha and blend with 0.f
    h->xa_->fcmle(p_tmp0.s, p_512 / T_z, vmm_src, vmm_aux0);
    h->xa_->mov(vmm_aux1, p_tmp0 / T_m, 0);

    h->xa_->mov(ZRegD(vmm_src.getIdx()), ZRegD(vmm_aux1.getIdx()));
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::pow_compute_vector_bwd(
        const TRegS &vmm_src) {
    // dispatch some special cases.
    if (beta_ == 0) { // zero
        /* This route has not been tested */
        h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(zero, z_tmp))));
    } else if (beta_ == 0.5) { // 0.5 * alpha / sqrt(s)
        sqrt_compute_vector_bwd(vmm_src);
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(alpha, z_tmp))));
    } else if (beta_ == 1) { // alpha
        h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(table_val(alpha, z_tmp))));
    } else {
        // Save `s` on stack for later usage
        h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);
        h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
        h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));
        // R = alpha * pow(s, beta)
        pow_compute_vector_fwd(vmm_src);
        // Restore `s` from stack
        h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
        h->ldr(ZReg(IDX(vmm_aux1)), ptr(h->X_TMP_0));
        h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);
        // Save mask of zero elements to convert them into zeros at the end
        if (beta_ >= 1)
            compute_cmp_mask(vmm_aux1, table_val(zero, z_tmp), _cmp_eq_oq);
        // res = alpha * beta * pow(s, beta - 1) = beta * R / s;
        h->xa_->fdiv(vmm_src, p_512, vmm_aux1);
        h->xa_->fmul(vmm_src, vmm_src, ZRegS(IDX(table_val(beta, z_tmp))));

        // beta < 1 leads to NaN as `s` appears in denominator, but beta >= 1
        // should lead to zero, when `s` is zero.
        if (beta_ >= 1) blend_with_mask(vmm_src, table_val(zero, z_tmp));
    }
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::gelu_erf_compute_vector_bwd(
        const TRegS &vmm_src) {
    // R = s / sqrt(2)
    h->xa_->fmul(vmm_src, vmm_src,
            ZRegS(IDX(table_val(gelu_erf_one_over_sqrt_two, z_tmp))));

    // Save R on stack for later usage
    h->sub_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->str(ZReg(IDX(vmm_src)), ptr(h->X_TMP_0));

    // Q = exp(-R*R)
    h->xa_->fmul(vmm_src, vmm_src, vmm_src);
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));
    exp_compute_vector_fwd(vmm_src);

    // T = R / sqrt(pi) * Q
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux2)), ptr(h->X_TMP_0));
    h->xa_->fmul(vmm_aux2, vmm_aux2,
            ZRegS(IDX(table_val(gelu_erf_one_over_sqrt_pi, z_tmp))));
    h->xa_->fmul(vmm_aux2, vmm_aux2, vmm_src);

    // -Q
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));

    // get sign
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux0)), ptr(h->X_TMP_0));
    h->xa_->and_(ZRegD(IDX(vmm_aux0)), ZRegD(IDX(vmm_aux0)),
            ZRegD(IDX(table_val(sign_mask, z_tmp))));

    // abs(x)
    h->add_imm(h->X_TMP_0, h->X_SP, 0, h->X_TMP_1);
    h->ldr(ZReg(IDX(vmm_aux1)), ptr(h->X_TMP_0));
    h->add_imm(h->X_SP, h->X_SP, vlen, h->X_TMP_0);

    abs_compute_vector_fwd(vmm_aux1);

    // W = 1 / (p * s + 1)
    h->xa_->mov(ZRegD(IDX(vmm_aux3)),
            ZRegD(IDX(table_val(gelu_erf_approx_const, z_tmp))));
    h->xa_->mov(ZRegD(IDX(vmm_aux4)), ZRegD(IDX(table_val(one, z_tmp))));
    h->fmad(vmm_aux3, p_512 / T_m, vmm_aux1, vmm_aux4);
    h->xa_->fdiv(vmm_aux4, p_512, vmm_aux3);

    // Q * W
    h->xa_->fmul(vmm_src, vmm_src, vmm_aux4);

    // compute polynomial r
    h->xa_->mov(ZRegD(IDX(vmm_aux1)),
            ZRegD(IDX(table_val(gelu_erf_pol, z_tmp, 4))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 3))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 2))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 1))));
    h->fmad(vmm_aux1, p_512 / T_m, vmm_aux4,
            ZRegS(IDX(table_val(gelu_erf_pol, z_tmp, 0))));

    // erf = sign * (1 - r * t * exp(-x*x))
    h->fmad(vmm_src, p_512 / T_m, vmm_aux1, ZRegS(IDX(table_val(one, z_tmp))));
    h->eor(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux0)));

    // P = T + 0.5
    h->xa_->fadd(vmm_aux2, vmm_aux2, ZRegS(IDX(table_val(half, z_tmp))));
    // res = P + 0.5 * erf
    h->fmla(vmm_aux2, p_512 / T_m, vmm_src, ZRegS(IDX(table_val(half, z_tmp))));
    h->xa_->mov(ZRegD(IDX(vmm_src)), ZRegD(IDX(vmm_aux2)));
}

template <cpu_isa_t isa>
size_t jit_uni_eltwise_injector_f32<isa>::aux_gprs_count() {
    using namespace alg_kind;
    switch (alg_) {
        case eltwise_tanh_use_dst_for_bwd:
        case eltwise_tanh:
        case eltwise_gelu_tanh: return 0;
        default: return 0;
    }
    return 0;
};

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::round_compute_vector_fwd(
        const TRegS &vmm_src) {
    h->frintn(vmm_src, p_512 / T_m, vmm_src);
}

template <cpu_isa_t isa>
size_t jit_uni_eltwise_injector_f32<isa>::aux_vecs_count() {
    using namespace alg_kind;

    if (is_fwd_) {
        switch (alg_) {
            case eltwise_relu_use_dst_for_bwd:
            case eltwise_relu: return (alpha_ == 0.f) ? 0 : 2;
            case eltwise_elu_use_dst_for_bwd:
            case eltwise_elu: return 6; /* = exp + 2 */
            case eltwise_tanh_use_dst_for_bwd:
            case eltwise_tanh: return 9;
            case eltwise_square: return 0;
            case eltwise_abs: return 1;
            case eltwise_sqrt_use_dst_for_bwd:
            case eltwise_sqrt: return 0;
            case eltwise_linear: return 2;
            case eltwise_bounded_relu: return 1;
            case eltwise_soft_relu: return 5;
            case eltwise_logistic_use_dst_for_bwd:
            case eltwise_logistic: return 5; /* = exp + 1 */
            case eltwise_exp_use_dst_for_bwd:
            case eltwise_exp: return 4;
            case eltwise_gelu_tanh: return 9; /* = tanh */
            case eltwise_swish: return 6; /* = logistic */
            case eltwise_log: return 5;
            case eltwise_clip: return 2;
            case eltwise_pow: return 3;
            case eltwise_gelu_erf: return 6;
            case eltwise_round: return 0;
            default: assert(!"unsupported eltwise algorithm");
        }
    } else {
        switch (alg_) {
            case eltwise_relu_use_dst_for_bwd:
            case eltwise_relu: return 1;
            case eltwise_elu_use_dst_for_bwd:
            case eltwise_elu: return 4; /* = exp */
            case eltwise_tanh_use_dst_for_bwd: return 2;
            case eltwise_tanh: return 9;
            case eltwise_square: return 1;
            case eltwise_abs: return 1;
            case eltwise_sqrt_use_dst_for_bwd:
            case eltwise_sqrt: return 2;
            case eltwise_linear: return 1;
            case eltwise_bounded_relu: return 1;
            case eltwise_soft_relu: return 5; /* = logistic */
            case eltwise_logistic_use_dst_for_bwd: return 2;
            case eltwise_logistic: return 5; /* = logistic */
            case eltwise_exp_use_dst_for_bwd: return 0;
            case eltwise_exp: return 4; /* = exp */
            case eltwise_gelu_tanh: return 9; /* = tanh */
            case eltwise_swish: return 6; /* = logistic */
            case eltwise_log: return 1;
            case eltwise_clip: return 3;
            case eltwise_pow: return 3;
            case eltwise_gelu_erf: return 6;
            default: assert(!"unsupported eltwise algorithm");
        }
    }

    return 0;
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::compute_body(
        const injector_utils::vmm_index_set_iterator_t &start_idx_it,
        const injector_utils::vmm_index_set_iterator_t &end_idx_it) {
    using namespace alg_kind;
    std::for_each(start_idx_it, end_idx_it, [&](size_t idx) {
        if (is_fwd_) {
            switch (alg_) {
                case eltwise_relu_use_dst_for_bwd:
                case eltwise_relu:
                    if (alpha_ == 0.f)
                        relu_zero_ns_compute_vector_fwd(TRegS(idx));
                    else
                        relu_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_elu_use_dst_for_bwd:
                case eltwise_elu: elu_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_tanh_use_dst_for_bwd:
                case eltwise_tanh: tanh_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_square:
                    square_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_abs: abs_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_sqrt_use_dst_for_bwd:
                case eltwise_sqrt: sqrt_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_swish: swish_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_linear:
                    linear_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_bounded_relu:
                    bounded_relu_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_soft_relu:
                    soft_relu_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_logistic_use_dst_for_bwd:
                case eltwise_logistic:
                    logistic_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_exp_use_dst_for_bwd:
                case eltwise_exp: exp_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_gelu_tanh:
                    gelu_tanh_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_log: log_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_clip: clip_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_pow: pow_compute_vector_fwd(TRegS(idx)); break;
                case eltwise_gelu_erf:
                    gelu_erf_compute_vector_fwd(TRegS(idx));
                    break;
                case eltwise_round: round_compute_vector_fwd(TRegS(idx)); break;
                default: assert(!"unsupported eltwise algorithm");
            }
        } else {
            switch (alg_) {
                case eltwise_relu_use_dst_for_bwd:
                case eltwise_relu: relu_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_elu_use_dst_for_bwd:
                case eltwise_elu: elu_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_tanh_use_dst_for_bwd:
                case eltwise_tanh: tanh_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_square:
                    square_compute_vector_bwd(TRegS(idx));
                    break;
                case eltwise_abs: abs_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_sqrt_use_dst_for_bwd:
                case eltwise_sqrt: sqrt_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_linear:
                    linear_compute_vector_bwd(TRegS(idx));
                    break;
                case eltwise_bounded_relu:
                    bounded_relu_compute_vector_bwd(TRegS(idx));
                    break;
                case eltwise_soft_relu:
                    soft_relu_compute_vector_bwd(TRegS(idx));
                    break;
                case eltwise_logistic_use_dst_for_bwd:
                case eltwise_logistic:
                    logistic_compute_vector_bwd(TRegS(idx));
                    break;
                case eltwise_exp_use_dst_for_bwd:
                case eltwise_exp: exp_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_gelu_tanh:
                    gelu_tanh_compute_vector_bwd(TRegS(idx));
                    break;
                case eltwise_swish: swish_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_log: log_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_clip: clip_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_pow: pow_compute_vector_bwd(TRegS(idx)); break;
                case eltwise_gelu_erf:
                    gelu_erf_compute_vector_bwd(TRegS(idx));
                    break;
                default: assert(!"unsupported eltwise algorithm");
            }
        }
        if (scale_ != 1.f) {
            h->xa_->fmul(ZRegS(IDX(TRegS(idx))), ZRegS(IDX(TRegS(idx))),
                    ZRegS(IDX(table_val(scale, z_tmp))));
        }
    });
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::compute_vector_range(
        size_t start_idx, size_t end_idx) {
    injector_utils::vmm_index_set_t vmm_idxs;
    for (size_t i = start_idx; i < end_idx; i++)
        vmm_idxs.emplace(i);
    compute_vector_range(vmm_idxs);
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::compute_vector_range(
        const injector_utils::vmm_index_set_t &vmm_idxs) {
    const auto &start_idx_it = vmm_idxs.begin();
    const auto &end_idx_it = vmm_idxs.end();
    assert(*start_idx_it < *vmm_idxs.rbegin() + 1
            && *vmm_idxs.rbegin() <= vecs_count);

    injector_preamble(vmm_idxs);
    compute_body(start_idx_tail, end_idx_it);
    injector_preamble_tail(start_idx_it);
    compute_body(start_idx_it, start_idx_tail);
    injector_postamble();
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::prepare_table(bool gen_table) {
    if (!gen_table) return;

    h->align(64);
    h->L(l_table);

    // Assumption: entries can be inserted with dd, so they should be 4 bytes.
    assert(sizeof(table_entry_val_t) == 4);

    // Assumption: iterating on entry_map_ here has the same order as
    // when we set the offsets. We verify that in asserts.
    // table_entry_val_t is assumed to be 32 bits
#ifndef NDEBUG
    size_t off = 0;
    key_t curr_key = undef_key;
    int key_occurences = 0;
#endif

    // Run through the map and insert values stored there
    for (auto it = entry_map_.begin(); it != entry_map_.end(); it++) {
        const auto &te = (*it).second; // get map entry for a given key
        const auto len = te.bcast ? vlen : sizeof(table_entry_val_t);
        /*        for (size_t d = 0; d < len; d += sizeof(table_entry_val_t))
            h->dd(te.val);*/
        for (size_t d = 0; d < len; d += sizeof(table_entry_val_t))
            h->Xbyak_aarch64::CodeArray::dw(te.val);

#ifndef NDEBUG
        // we check that the precomputed offsets match the registered ones
        const auto &key = (*it).first; // get map entry key
        if (key != curr_key) {
            curr_key = key;
            key_occurences = 0;
        }
        key_occurences++;
        auto expected_off = table_off(key, key_occurences - 1);
        assert(off == expected_off);
        MAYBE_UNUSED(expected_off);
        off += len;
#endif
    }
}

template <cpu_isa_t isa>
void jit_uni_eltwise_injector_f32<isa>::register_table_entries() {
    // This function is responsible to pick all necessary constants
    // for a given algorithm, compute right offset for them to be used
    // in table_val() and save the hexadecimal value of them, which
    // will be finally used in prepare_table(). We rely on fact that
    // the map iterator order is deterministic for a fixed map.

    // common values used in several algorithms
    static const table_t common_values {{zero, {0x00000000, true}},
            {half, {0x3f000000, true}}, {one, {0x3f800000, true}},
            {two, {0x40000000, true}}, {minus_one, {0xbf800000, true}},
            {minus_two, {0xc0000000, true}}, {ln2f, {0x3f317218, true}},
            {positive_mask, {0x7fffffff, true}},
            {sign_mask, {0x80000000, true}},
            {exponent_bias, {0x0000007f, true}}};

    // exp(x) constants
    static const table_t exp_consts {{exp_log2ef, {0x3fb8aa3b, true}},
            {exp_ln_flt_max_f, {0x42b17218, true}},
            {exp_ln_flt_min_f, {0xc2aeac50, true}}};

    // exp(x) polynomial approximation
    static const table_t exp_polynomial {
            {exp_pol, {0x3f7ffffb, true}}, // p1 = 0.999999701f
            {exp_pol, {0x3efffee3, true}}, // p2 = 0.499991506f
            {exp_pol, {0x3e2aad40, true}}, // p3 = 0.166676521f
            {exp_pol, {0x3d2b9d0d, true}}, // p4 = 0.0418978221f
            {exp_pol, {0x3c07cfce, true}} // p5 = 0.00828929059f
    };

    // tanh(x) constants for four interval approximation
    static const table_t tanh_consts {{tanh_idx_bias, {0x39800000, true}},
            {tanh_idx_mask, {0xffc00000, true}},
            {tanh_linear_ubound, {0x39ddb3d7, true}},
            {tanh_saturation_lbound, {0x41102cb3, true}}};

    // tanh(x) polynomial approximation
    // For each coefficient, there is 32 entries
    static const table_t tanh_polynomial_table {
            // coefficients of degree 0
            {tanh_pol_table, {0x00000000, false}},
            {tanh_pol_table, {0x39bfffff, false}},
            {tanh_pol_table, {0x39ffffff, false}},
            {tanh_pol_table, {0x3a3ffffe, false}},
            {tanh_pol_table, {0x3a7ffffb, false}},
            {tanh_pol_table, {0x3abffff7, false}},
            {tanh_pol_table, {0x3affffeb, false}},
            {tanh_pol_table, {0x3b3fffdc, false}},
            {tanh_pol_table, {0x3b7fffab, false}},
            {tanh_pol_table, {0x3bbfff70, false}},
            {tanh_pol_table, {0x3bfffeab, false}},
            {tanh_pol_table, {0x3c3ffdc0, false}},
            {tanh_pol_table, {0x3c7ffaab, false}},
            {tanh_pol_table, {0x3cbff701, false}},
            {tanh_pol_table, {0x3cffeaad, false}},
            {tanh_pol_table, {0x3d3fdc08, false}},
            {tanh_pol_table, {0x3d7faacd, false}},
            {tanh_pol_table, {0x3dbf7081, false}},
            {tanh_pol_table, {0x3dfeacc9, false}},
            {tanh_pol_table, {0x3e3dc7fd, false}},
            {tanh_pol_table, {0x3e7acbf5, false}},
            {tanh_pol_table, {0x3eb77a9f, false}},
            {tanh_pol_table, {0x3eec9a9f, false}},
            {tanh_pol_table, {0x3f22991f, false}},
            {tanh_pol_table, {0x3f42f7d6, false}},
            {tanh_pol_table, {0x3f67b7cc, false}},
            {tanh_pol_table, {0x3f76ca83, false}},
            {tanh_pol_table, {0x3f7ebbe9, false}},
            {tanh_pol_table, {0x3f7fd40c, false}},
            {tanh_pol_table, {0x3f7fff32, false}},
            {tanh_pol_table, {0x3f7ffffc, false}},
            {tanh_pol_table, {0x3f800000, false}},
            // coefficients of degree 1
            {tanh_pol_table, {0x3f800000, false}},
            {tanh_pol_table, {0x3f800018, false}},
            {tanh_pol_table, {0x3f7fffe8, false}},
            {tanh_pol_table, {0x3f7fffda, false}},
            {tanh_pol_table, {0x3f7fffdc, false}},
            {tanh_pol_table, {0x3f7fffdc, false}},
            {tanh_pol_table, {0x3f7fffac, false}},
            {tanh_pol_table, {0x3f7fff70, false}},
            {tanh_pol_table, {0x3f7ffeec, false}},
            {tanh_pol_table, {0x3f7ffdc0, false}},
            {tanh_pol_table, {0x3f7ffbed, false}},
            {tanh_pol_table, {0x3f7ff704, false}},
            {tanh_pol_table, {0x3f7feff5, false}},
            {tanh_pol_table, {0x3f7fdbca, false}},
            {tanh_pol_table, {0x3f7fbfff, false}},
            {tanh_pol_table, {0x3f7f7041, false}},
            {tanh_pol_table, {0x3f7f009b, false}},
            {tanh_pol_table, {0x3f7dc36c, false}},
            {tanh_pol_table, {0x3f7c0aa8, false}},
            {tanh_pol_table, {0x3f7734b8, false}},
            {tanh_pol_table, {0x3f70a4de, false}},
            {tanh_pol_table, {0x3f5f1fd8, false}},
            {tanh_pol_table, {0x3f495493, false}},
            {tanh_pol_table, {0x3f18b9ec, false}},
            {tanh_pol_table, {0x3ed706cb, false}},
            {tanh_pol_table, {0x3e390b06, false}},
            {tanh_pol_table, {0x3d90b11f, false}},
            {tanh_pol_table, {0x3c21a053, false}},
            {tanh_pol_table, {0x3aaf7fdb, false}},
            {tanh_pol_table, {0x37ccc1a3, false}},
            {tanh_pol_table, {0x355c6733, false}},
            {tanh_pol_table, {0x00000000, false}},
            // coefficients of degree 2
            {tanh_pol_table, {0x00000000, false}},
            {tanh_pol_table, {0xbe4e0ff1, false}},
            {tanh_pol_table, {0x3d25b1b1, false}},
            {tanh_pol_table, {0x3d6b6dab, false}},
            {tanh_pol_table, {0x3c9fb1d5, false}},
            {tanh_pol_table, {0xbabff06f, false}},
            {tanh_pol_table, {0x3c07b3f6, false}},
            {tanh_pol_table, {0xbb3fc1bc, false}},
            {tanh_pol_table, {0x3a9f5921, false}},
            {tanh_pol_table, {0xbbbf06f2, false}},
            {tanh_pol_table, {0xbbb0f402, false}},
            {tanh_pol_table, {0xbc47db9e, false}},
            {tanh_pol_table, {0xbc73d5e7, false}},
            {tanh_pol_table, {0xbca25bda, false}},
            {tanh_pol_table, {0xbcfca780, false}},
            {tanh_pol_table, {0xbd40e07c, false}},
            {tanh_pol_table, {0xbd7dab03, false}},
            {tanh_pol_table, {0xbdbe4a0f, false}},
            {tanh_pol_table, {0xbdfb14a5, false}},
            {tanh_pol_table, {0xbe36cc8d, false}},
            {tanh_pol_table, {0xbe6bd102, false}},
            {tanh_pol_table, {0xbe9fe7c5, false}},
            {tanh_pol_table, {0xbeba0f10, false}},
            {tanh_pol_table, {0xbec206a8, false}},
            {tanh_pol_table, {0xbea3c388, false}},
            {tanh_pol_table, {0xbe277d62, false}},
            {tanh_pol_table, {0xbd8b7960, false}},
            {tanh_pol_table, {0xbc209f49, false}},
            {tanh_pol_table, {0xbaad44ca, false}},
            {tanh_pol_table, {0xb7c6eeac, false}},
            {tanh_pol_table, {0xb663aa41, false}},
            {tanh_pol_table, {0x00000000, false}},
            // coefficients of degree 3
            {tanh_pol_table, {0x00000000, false}},
            {tanh_pol_table, {0x45b3ae96, false}},
            {tanh_pol_table, {0xc414eb20, false}},
            {tanh_pol_table, {0xc450e02e, false}},
            {tanh_pol_table, {0xc3152b4e, false}},
            {tanh_pol_table, {0xbead2f56, false}},
            {tanh_pol_table, {0xc2162e02, false}},
            {tanh_pol_table, {0xbeb4bd5a, false}},
            {tanh_pol_table, {0xc11a59a4, false}},
            {tanh_pol_table, {0xbed2f507, false}},
            {tanh_pol_table, {0xc020d32c, false}},
            {tanh_pol_table, {0x3dd0f506, false}},
            {tanh_pol_table, {0xbf2a75e2, false}},
            {tanh_pol_table, {0xbff950e3, false}},
            {tanh_pol_table, {0xbed47334, false}},
            {tanh_pol_table, {0xbe809b8c, false}},
            {tanh_pol_table, {0xbeb64532, false}},
            {tanh_pol_table, {0xbe961a5b, false}},
            {tanh_pol_table, {0xbe9b63ac, false}},
            {tanh_pol_table, {0xbea0d4b2, false}},
            {tanh_pol_table, {0xbe828a77, false}},
            {tanh_pol_table, {0xbe378612, false}},
            {tanh_pol_table, {0xbdc20908, false}},
            {tanh_pol_table, {0x3d2d3957, false}},
            {tanh_pol_table, {0x3dd46e89, false}},
            {tanh_pol_table, {0x3db3f629, false}},
            {tanh_pol_table, {0x3d2c5e7b, false}},
            {tanh_pol_table, {0x3bd20403, false}},
            {tanh_pol_table, {0x3a59dfae, false}},
            {tanh_pol_table, {0x3770af45, false}},
            {tanh_pol_table, {0x372cc014, false}},
            {tanh_pol_table, {0x00000000, false}},
            // coefficients of degree 4
            {tanh_pol_table, {0x00000000, false}},
            {tanh_pol_table, {0xcc981a1b, false}},
            {tanh_pol_table, {0x4a7edd3d, false}},
            {tanh_pol_table, {0x4ab1007c, false}},
            {tanh_pol_table, {0x48fedd9c, false}},
            {tanh_pol_table, {0x41a557b5, false}},
            {tanh_pol_table, {0x477ee32a, false}},
            {tanh_pol_table, {0x422557f5, false}},
            {tanh_pol_table, {0x45ff3ce4, false}},
            {tanh_pol_table, {0x42a55641, false}},
            {tanh_pol_table, {0x446e0867, false}},
            {tanh_pol_table, {0xc33dc19a, false}},
            {tanh_pol_table, {0x42915214, false}},
            {tanh_pol_table, {0x43af4fad, false}},
            {tanh_pol_table, {0x4110fe88, false}},
            {tanh_pol_table, {0xc1099b75, false}},
            {tanh_pol_table, {0x3fc8a8dc, false}},
            {tanh_pol_table, {0xbfbeaef5, false}},
            {tanh_pol_table, {0xbe365aad, false}},
            {tanh_pol_table, {0x3f4d9652, false}},
            {tanh_pol_table, {0x3ddfa08f, false}},
            {tanh_pol_table, {0x3e34e9b8, false}},
            {tanh_pol_table, {0x3e2d07a6, false}},
            {tanh_pol_table, {0x3dc63567, false}},
            {tanh_pol_table, {0x3cdaeb78, false}},
            {tanh_pol_table, {0xbcd17537, false}},
            {tanh_pol_table, {0xbc92829c, false}},
            {tanh_pol_table, {0xbb43ab99, false}},
            {tanh_pol_table, {0xb9b471dd, false}},
            {tanh_pol_table, {0xb6baad5a, false}},
            {tanh_pol_table, {0xb78bafc7, false}},
            {tanh_pol_table, {0x00000000, false}},
            // coefficients of degree 5
            {tanh_pol_table, {0x00000000, false}},
            {tanh_pol_table, {0x52f688d5, false}},
            {tanh_pol_table, {0xd0505c72, false}},
            {tanh_pol_table, {0xd08f98e3, false}},
            {tanh_pol_table, {0xce505cc9, false}},
            {tanh_pol_table, {0xc7162b8a, false}},
            {tanh_pol_table, {0xcc5061d6, false}},
            {tanh_pol_table, {0xc7162bdf, false}},
            {tanh_pol_table, {0xca50b37f, false}},
            {tanh_pol_table, {0xc7162a3a, false}},
            {tanh_pol_table, {0xc8422086, false}},
            {tanh_pol_table, {0x471a714e, false}},
            {tanh_pol_table, {0xc5ece1f1, false}},
            {tanh_pol_table, {0xc70e3d90, false}},
            {tanh_pol_table, {0xc3eba94a, false}},
            {tanh_pol_table, {0x43e0c424, false}},
            {tanh_pol_table, {0xc21f4552, false}},
            {tanh_pol_table, {0x42217cc8, false}},
            {tanh_pol_table, {0x405e7dc4, false}},
            {tanh_pol_table, {0xc10dd401, false}},
            {tanh_pol_table, {0x3e96b602, false}},
            {tanh_pol_table, {0xbd1a6d2f, false}},
            {tanh_pol_table, {0xbd393883, false}},
            {tanh_pol_table, {0xbd674682, false}},
            {tanh_pol_table, {0xbd310016, false}},
            {tanh_pol_table, {0xb961e269, false}},
            {tanh_pol_table, {0x3ba32495, false}},
            {tanh_pol_table, {0x3a7680d5, false}},
            {tanh_pol_table, {0x38b3173c, false}},
            {tanh_pol_table, {0x35a9deea, false}},
            {tanh_pol_table, {0x375c3f2a, false}},
            {tanh_pol_table, {0x00000000, false}},
            // coefficients of degree 6
            {tanh_pol_table, {0x00000000, false}},
            {tanh_pol_table, {0xd8995ed1, false}},
            {tanh_pol_table, {0x558285ea, false}},
            {tanh_pol_table, {0x55b2cd69, false}},
            {tanh_pol_table, {0x53028625, false}},
            {tanh_pol_table, {0x4bc9991f, false}},
            {tanh_pol_table, {0x5082898a, false}},
            {tanh_pol_table, {0x4b4999b3, false}},
            {tanh_pol_table, {0x4e02c07c, false}},
            {tanh_pol_table, {0x4ac99764, false}},
            {tanh_pol_table, {0x4b72c822, false}},
            {tanh_pol_table, {0xca40c0e1, false}},
            {tanh_pol_table, {0x489413e4, false}},
            {tanh_pol_table, {0x49b12224, false}},
            {tanh_pol_table, {0x46134c4e, false}},
            {tanh_pol_table, {0xc60c2d57, false}},
            {tanh_pol_table, {0x43c83910, false}},
            {tanh_pol_table, {0xc3c872d1, false}},
            {tanh_pol_table, {0xc186bc9e, false}},
            {tanh_pol_table, {0x42325bc3, false}},
            {tanh_pol_table, {0xbf2ffa4a, false}},
            {tanh_pol_table, {0x3d9a203c, false}},
            {tanh_pol_table, {0xbc545a43, false}},
            {tanh_pol_table, {0xbae08fee, false}},
            {tanh_pol_table, {0x3c80225d, false}},
            {tanh_pol_table, {0x3b1fd1df, false}},
            {tanh_pol_table, {0xba36b9d1, false}},
            {tanh_pol_table, {0xb91de544, false}},
            {tanh_pol_table, {0xb71f100f, false}},
            {tanh_pol_table, {0xb408e2ed, false}},
            {tanh_pol_table, {0xb685fec8, false}},
            {tanh_pol_table, {0x00000000, false}},
    };

    // soft_relu(x) constants
    static const table_t soft_relu_consts {
            {soft_relu_one_twenty_six, {0x42fc0000, true}},
            {soft_relu_mantissa_sign_mask, {0x807fffff, true}},
    };

    // soft_relu ln(1 + x) polynomial approximation
    static const table_t soft_relu_polynomial {
            {soft_relu_pol, {0xb2b4637d, true}}, // p0 = 0.0000000244f
            {soft_relu_pol, {0x3f7fff8e, true}}, // p1 = 0.9999976971f
            {soft_relu_pol, {0xbf001759, true}}, // p2 = -0.5002478215f
            {soft_relu_pol, {0x3ea70608, true}}, // p3 = 0.3272714505f
            {soft_relu_pol, {0xbea3d7bf, true}}, // p4 = -0.3153830071f
            {soft_relu_pol, {0xbe361d04, true}}, // p5 = -0.1701777461f
            {soft_relu_pol, {0xbfa8f1e6, true}}, // p6 = -1.3254635147f
            {soft_relu_pol, {0xbfe1e812, true}}, // p7 = -1.7971917960f
            {soft_relu_pol, {0xbfc4d30e, true}}, // p8 = -1.5652673123f
    };

    // gelu_tanh(x) constants (formula defined)
    static const table_t gelu_tanh_consts {
            {gelu_tanh_fitting_const, {0x3d372713, true}},
            {gelu_tanh_fitting_const_times_three, {0x3e095d4f, true}},
            {gelu_tanh_sqrt_two_over_pi, {0x3f4c422a, true}},
    };

    // gelu_erf(x) constants (formula defined)
    static const table_t gelu_erf_consts {
            {gelu_erf_approx_const, {0x3ea7ba05, true}},
            {gelu_erf_one_over_sqrt_two, {0x3f3504f3, true}},
            {gelu_erf_one_over_sqrt_pi, {0x3f106eba, true}},
    };

    // gelu_erf(x) polynomial approximation
    static const table_t gelu_erf_polynomial {
            {gelu_erf_pol, {0x3e827906, true}}, // p1 = 0.254829592f
            {gelu_erf_pol, {0xbe91a98e, true}}, // p2 = -0.284496736f
            {gelu_erf_pol, {0x3fb5f0e3, true}}, // p3 = 1.421413741f
            {gelu_erf_pol, {0xbfba00e3, true}}, // p4 = -1.453152027f
            {gelu_erf_pol, {0x3f87dc22, true}}, // p5 = 1.061405429f
    };

    // log(x) constants
    static const table_t log_consts {
            {log_minus_inf, {0xff800000, true}},
            {log_qnan, {0x7fc00000, true}},
            {log_mantissa_mask, {0x007fffff, true}},
            {log_full_k_reg_mask, {0x0000ffff, true}},
            {log_five_bit_offset, {0x0000001f, true}},
    };

    // log(x) polynomial approximation
    static const table_t log_polynomial {
            {log_pol, {0xbf000000, true}}, // p1 = -0.5f
            {log_pol, {0x3eaaaaab, true}}, // p2 =  0.333333343f
            {log_pol, {0xbe8004ab, true}}, // p3 = -0.250035613f
            {log_pol, {0x3e4cc8a3, true}}, // p4 =  0.199984118f
    };

    // log(x) pre-defined values. First goes index}, then val[index].
    static const table_t log_predefined_values {
            {log_predefined_vals, {0x3f800000, true}}, //  0: 1
            {log_predefined_vals,
                    {0xc2b00f34, true}}, //  1: -88.029693603515625
            {log_predefined_vals, {0x3f780000, true}}, //  2: 0.96875
            {log_predefined_vals,
                    {0xc2affef2, true}}, //  3: -87.9979400634765625
            {log_predefined_vals, {0x3f700000, true}}, //  4: 0.9375
            {log_predefined_vals,
                    {0xc2afee29, true}}, //  5: -87.9651565551757812
            {log_predefined_vals, {0x3f680000, true}}, //  6: 0.90625
            {log_predefined_vals,
                    {0xc2afdccd, true}}, //  7: -87.9312515258789062
            {log_predefined_vals, {0x3f600000, true}}, //  8: 0.875
            {log_predefined_vals,
                    {0xc2afcad6, true}}, //  9: -87.8961639404296875
            {log_predefined_vals, {0x3f580000, true}}, // 10: 0.84375
            {log_predefined_vals,
                    {0xc2afb837, true}}, // 11: -87.859794616699218
            {log_predefined_vals, {0x3f580000, true}}, // 12: 0.84375
            {log_predefined_vals,
                    {0xc2afb837, true}}, // 13: -87.859794616699218
            {log_predefined_vals, {0x3f500000, true}}, // 14: 0.8125
            {log_predefined_vals,
                    {0xc2afa4e4, true}}, // 15: -87.822052001953125
            {log_predefined_vals, {0x3f480000, true}}, // 16: 0.78125
            {log_predefined_vals,
                    {0xc2af90cf, true}}, // 17: -87.782829284667968
            {log_predefined_vals, {0x3f480000, true}}, // 18: 0.78125
            {log_predefined_vals,
                    {0xc2af90cf, true}}, // 19: -87.782829284667968
            {log_predefined_vals, {0x3f400000, true}}, // 20: 0.75
            {log_predefined_vals,
                    {0xc2af7be9, true}}, // 21: -87.742012023925781
            {log_predefined_vals, {0x3f400000, true}}, // 22: 0.75
            {log_predefined_vals,
                    {0xc2af7be9, true}}, // 23: -87.742012023925781
            {log_predefined_vals, {0x3f380000, true}}, // 24: 0.71875
            {log_predefined_vals,
                    {0xc2af661e, true}}, // 25: -87.699447631835937
            {log_predefined_vals, {0x3f380000, true}}, // 26: 0.71875
            {log_predefined_vals,
                    {0xc2af661e, true}}, // 27: -87.699447631835937
            {log_predefined_vals, {0x3f300000, true}}, // 28: 0.6875
            {log_predefined_vals,
                    {0xc2af4f5c, true}}, // 29: -87.654998779296875
            {log_predefined_vals, {0x3f300000, true}}, // 30: 0.6875
            {log_predefined_vals,
                    {0xc2af4f5c, true}}, // 31: -87.654998779296875
            {log_predefined_vals, {0x3fa80000, true}}, // 32: 1.3125
            {log_predefined_vals,
                    {0xc2b09a6f, true}}, // 33: -88.301628112792968
            {log_predefined_vals, {0x3fa80000, true}}, // 34: 1.3125
            {log_predefined_vals,
                    {0xc2b09a6f, true}}, // 35: -88.301628112792968
            {log_predefined_vals, {0x3fa00000, true}}, // 36: 1.25
            {log_predefined_vals,
                    {0xc2b08174, true}}, // 37: -88.252838134765625
            {log_predefined_vals, {0x3fa00000, true}}, // 38: 1.25
            {log_predefined_vals,
                    {0xc2b08174, true}}, // 39: -88.252838134765625
            {log_predefined_vals, {0x3fa00000, true}}, // 40: 1.25
            {log_predefined_vals,
                    {0xc2b08174, true}}, // 41: -88.252838134765625
            {log_predefined_vals, {0x3f980000, true}}, // 42: 1.1875
            {log_predefined_vals,
                    {0xc2b06731, true}}, // 43: -88.201545715332031
            {log_predefined_vals, {0x3f980000, true}}, // 44: 1.1875
            {log_predefined_vals,
                    {0xc2b06731, true}}, // 45: -88.201545715332031
            {log_predefined_vals, {0x3f900000, true}}, // 46: 1.125
            {log_predefined_vals,
                    {0xc2b04b82, true}}, // 47: -88.147476196289062
            {log_predefined_vals, {0x3f900000, true}}, // 48: 1.125
            {log_predefined_vals,
                    {0xc2b04b82, true}}, // 49: -88.147476196289062
            {log_predefined_vals, {0x3f900000, true}}, // 50: 1.125
            {log_predefined_vals,
                    {0xc2b04b82, true}}, // 51: -88.147476196289062
            {log_predefined_vals, {0x3f900000, true}}, // 52: 1.125
            {log_predefined_vals,
                    {0xc2b04b82, true}}, // 53: -88.147476196289062
            {log_predefined_vals, {0x3f880000, true}}, // 54: 1.0625
            {log_predefined_vals,
                    {0xc2b02e3e, true}}, // 55: -88.090316772460937
            {log_predefined_vals, {0x3f880000, true}}, // 56: 1.0625
            {log_predefined_vals,
                    {0xc2b02e3e, true}}, // 57: -88.090316772460937
            {log_predefined_vals, {0x3f880000, true}}, // 58: 1.0625
            {log_predefined_vals,
                    {0xc2b02e3e, true}}, // 59: -88.090316772460937
            {log_predefined_vals, {0x3f800000, true}}, // 60: 1
            {log_predefined_vals,
                    {0xc2b00f34, true}}, // 61: -88.029693603515625
            {log_predefined_vals, {0x3f800000, true}}, // 62: 1
            {log_predefined_vals,
                    {0xc2b00f34, true}}, // 63: -88.029693603515625
    };

    // This object takes care about which constants and polynomials to include.
    struct need_t {
        need_t(alg_kind_t alg) {
            using namespace alg_kind;
            switch (alg) {
                case eltwise_elu_use_dst_for_bwd:
                case eltwise_elu:
                case eltwise_exp_use_dst_for_bwd:
                case eltwise_exp:
                case eltwise_logistic_use_dst_for_bwd:
                case eltwise_logistic:
                case eltwise_swish: exp_ = true; break;
                case eltwise_gelu_erf: gelu_erf_ = true; break;
                case eltwise_gelu_tanh: gelu_tanh_ = true; break;
                case eltwise_log: log_ = true; break;
                case eltwise_soft_relu: soft_relu_ = true; break;
                case eltwise_tanh_use_dst_for_bwd:
                case eltwise_tanh: tanh_ = true; break;
                default: break;
            }
        }

        bool exp_ = false;
        bool tanh_ = false;
        bool soft_relu_ = false;
        bool gelu_tanh_ = false;
        bool gelu_erf_ = false;
        bool log_ = false;

        bool exp() const { return exp_ || soft_relu_ || gelu_erf_; }
        bool tanh() const { return tanh_ || gelu_tanh_; }
        bool soft_relu() const { return soft_relu_; }
        bool gelu_tanh() const { return gelu_tanh_; }
        bool gelu_erf() const { return gelu_erf_; }
        bool log() const { return log_; }
    };

    need_t need(alg_);

    auto push_arg_entry_of = [&](const key_t key, const table_entry_val_t val,
                                     const bool broadcast) {
        mapped_table_entry_t te {0, val, broadcast};
        entry_map_.insert(std::make_pair(key, te));
    };

    auto push_entries_of = [&](const table_t &t) {
        for (auto it = t.begin(); it != t.end(); it++) {
            auto key = (*it).first;
            auto te = (*it).second; // copy values from table
            push_arg_entry_of(key, te.val, te.bcast);
        }
    };

    push_arg_entry_of(scale, float2int(scale_), true);
    push_arg_entry_of(alpha, float2int(alpha_), true);
    push_arg_entry_of(beta, float2int(beta_), true);
    push_entries_of(common_values);
    if (need.exp()) push_entries_of(exp_consts);
    if (need.exp()) push_entries_of(exp_polynomial);
    if (need.tanh()) push_entries_of(tanh_consts);
    if (need.tanh()) push_entries_of(tanh_polynomial_table);
    if (need.soft_relu()) push_entries_of(soft_relu_consts);
    if (need.soft_relu()) push_entries_of(soft_relu_polynomial);
    if (need.gelu_tanh()) push_entries_of(gelu_tanh_consts);
    if (need.gelu_erf()) push_entries_of(gelu_erf_consts);
    if (need.gelu_erf()) push_entries_of(gelu_erf_polynomial);
    if (need.log()) push_entries_of(log_consts);
    if (need.log()) push_entries_of(log_polynomial);
    if (need.log()) push_entries_of(log_predefined_values);

    // Now that we registered the entries, we set the offsets.  No
    // entries should be registered after this point.  This allows to
    // expect the same order when injecting the table entries in
    // prepare_table.
    size_t off = 0;
    for (auto it = entry_map_.begin(); it != entry_map_.end(); it++) {
        auto &te = (*it).second;
        te.off = off;
        off += te.bcast ? vlen : sizeof(table_entry_val_t);
    }
}

template struct jit_uni_eltwise_injector_f32<sve_512>;
template struct jit_uni_eltwise_injector_f32<avx512_common>;

} // namespace aarch64
} // namespace cpu
} // namespace impl
} // namespace dnnl
