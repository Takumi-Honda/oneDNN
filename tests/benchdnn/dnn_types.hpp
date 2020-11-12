/*******************************************************************************
* Copyright 2017-2020 Intel Corporation
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

#ifndef DNN_TYPES_HPP
#define DNN_TYPES_HPP

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "common.hpp"
#include "dnnl_types.h"

namespace tag {
extern const char *abx;
extern const char *any;
extern const char *undef;
} // namespace tag

struct dims_t : public std::vector<int64_t> {};
enum dir_t {
    DIR_UNDEF = 0,
    FLAG_DAT = 1,
    FLAG_WEI = 2,
    FLAG_BIA = 4,
    FLAG_FWD = 32,
    FLAG_BWD = 64,
    FLAG_INF = 128,
    FWD_D = FLAG_FWD + FLAG_DAT,
    FWD_I = FLAG_FWD + FLAG_DAT + FLAG_INF,
    FWD_B = FLAG_FWD + FLAG_DAT + FLAG_BIA,
    BWD_D = FLAG_BWD + FLAG_DAT,
    BWD_DW = FLAG_BWD + FLAG_DAT + FLAG_WEI,
    BWD_W = FLAG_BWD + FLAG_WEI,
    BWD_WB = FLAG_BWD + FLAG_WEI + FLAG_BIA,
};
dir_t str2dir(const char *str);

/* TODO: merge prop and dir_t (in favor of prop) */
const char *prop2str(dnnl_prop_kind_t prop);
dnnl_prop_kind_t prop2prop_kind(dir_t dir);

dims_t off2dims_idx(const dims_t &dims, int64_t off);
std::ostream &operator<<(std::ostream &s, const dims_t &dims);
std::ostream &operator<<(std::ostream &s, dir_t dir);
std::ostream &operator<<(std::ostream &s, dnnl_data_type_t dt);
template <typename T>
std::ostream &operator<<(std::ostream &s, const std::vector<T> &v) {
    s << v[0];
    for (size_t d = 1; d < v.size(); ++d)
        s << ":" << v[d];
    return s;
}

typedef int data_kind_t;
enum { SRC = 0, WEI, BIA, DST, ACC, DATA, MEAN, VAR, SS, GWEI, DAT_TOTAL };
const char *data_kind2str(data_kind_t kind);

struct attr_t {
    struct scale_t {
        enum policy_t {
            COMMON = 0,
            PER_OC,
            // reorder section
            // XXX: order is important, from longer name to a shorter one
            // TODO: generalize, use numbers instead of predefined enum
            PER_DIM_01,
            PER_DIM_0,
            PER_DIM_1,
            // reorder section ends
            POLICY_TOTAL
        };
        scale_t() = default;
        scale_t(policy_t policy, float scale) : policy(policy), scale(scale) {}

        static policy_t str2policy(const char *str);
        static const char *policy2str(policy_t policy);

        int from_str(const char *str, const char **end_s);

        bool is_def() const { return policy == COMMON && scale == 1.; }

        static int get_default_mask(policy_t policy) {
            switch (policy) {
                case PER_DIM_0: return (1 << 0);
                case PER_OC:
                case PER_DIM_1: return (1 << 1);
                case PER_DIM_01: return (1 << 0) + (1 << 1);
                case COMMON: return 0;
                default: SAFE_V(FAIL); return 0;
            }
        }

        policy_t policy = COMMON;
        float scale = 1.;
        bool runtime = false;
    };

    struct zero_points_t {
        struct entry_t {
            int value;
            bool runtime;
        };

        int from_str(const char *str, const char **end_s);

        int operator[](int arg) const { return get(arg).value; }
        bool runtime(int arg) const { return get(arg).runtime; }

        bool is_def() const { return points.empty(); }

        void set(int arg, const entry_t &entry) {
            if (entry.value != 0 || entry.runtime) points[arg] = entry;
        }
        entry_t get(int arg) const {
            const auto it = points.find(arg);
            return it == points.end() ? entry_t() : it->second;
        }

        std::map<int, entry_t>::const_iterator begin() const {
            return points.begin();
        }
        std::map<int, entry_t>::const_iterator end() const {
            return points.end();
        }

        zero_points_t() : points() {} // needed for debug icc190 build;

        std::map<int, entry_t> points;
    };

    struct arg_scales_t {
        void set(int arg, scale_t scale) {
            scales.insert(std::make_pair(arg, scale));
        }

        scale_t get(int arg) const {
            const auto &s = scales.find(arg);
            return s == scales.end() ? scale_t() : s->second;
        }

        bool is_def() const { return scales.empty(); }
        int from_str(const char *str, const char **end_s);

        arg_scales_t() : scales() {} // needed for debug icc190 build;

        std::map<int, scale_t> scales;
    };

    struct post_ops_t {
        enum kind_t {
            // sum
            SUM,
            // depthwise convolution
            DW_K3S1P1,
            DW_K3S2P1,
            // eltwise
            ELTWISE_START, // a guard to check kind is eltwise
            ABS,
            BRELU,
            CLIP,
            ELU,
            ELU_DST,
            EXP,
            EXP_DST,
            GELU_ERF,
            GELU_TANH,
            LINEAR,
            LOG,
            LOGISTIC,
            LOGISTIC_DST,
            POW,
            RELU,
            RELU_DST,
            ROUND,
            SQRT,
            SQRT_DST,
            SQUARE,
            SRELU,
            SWISH,
            TANH,
            TANH_DST,
            ELTWISE_END, // a guard to check kind is eltwise
            // binary
            BINARY_START, // a guard to check kind is binary
            ADD,
            MAX,
            MIN,
            MUL,
            BINARY_END, // a guard to check kind is binary
            // guard entry
            KIND_TOTAL
        };
        static kind_t str2kind(const char *str);
        static const char *kind2str(kind_t kind);
        static dnnl_alg_kind_t kind2dnnl_kind(kind_t kind);

        struct entry_t {
            entry_t() {}

            kind_t kind;
            union {
                struct {
                    float scale;
                    dnnl_data_type_t dt;
                } sum;
                struct {
                    dnnl_alg_kind_t alg;
                    float scale, alpha, beta;
                } eltwise;
                struct {
                    int stride;
                    dnnl_data_type_t dst_dt;
                    scale_t oscale;
                } convolution;
            };

            bool is_eltwise_kind() const;
            bool is_convolution_kind() const;
        };

        post_ops_t() : len(0) {}

        int from_str(const char *str, const char **end_s);
        void to_str(char *buffer, char **end_b) const;

        bool is_def() const { return len == 0; }
        int find(kind_t kind, int start = 0, int stop = -1) const;
        int eltwise_index() const;
        int convolution_index() const;

        enum { capacity = 4 };
        int len;
        entry_t entry[4];
    };

    attr_t() = default;

    attr_t(const post_ops_t &po) : post_ops(po) {}

    attr_t(const scale_t &s, const post_ops_t &po) : oscale(s), post_ops(po) {}

    attr_t(const arg_scales_t &as, const post_ops_t &po)
        : scales(as), post_ops(po) {}

    attr_t(const scale_t &s, const zero_points_t &zp, const post_ops_t &po)
        : oscale(s), zero_points(zp), post_ops(po) {}

    scale_t oscale;
    arg_scales_t scales;
    zero_points_t zero_points;
    post_ops_t post_ops;

    bool is_def() const;
};
using policy_t = attr_t::scale_t::policy_t;

void handle_legacy_attr(attr_t &attr, const attr_t &legacy_attr);

int str2attr(attr_t *attr, const char *str);
std::ostream &operator<<(std::ostream &s, const policy_t &policy);
std::ostream &operator<<(std::ostream &s, const attr_t::scale_t &scale);
std::ostream &operator<<(
        std::ostream &s, const attr_t::zero_points_t &zero_points);
std::ostream &operator<<(std::ostream &s, const attr_t::arg_scales_t &scales);
std::ostream &operator<<(std::ostream &s, const attr_t::post_ops_t::kind_t &k);
std::ostream &operator<<(std::ostream &s, const attr_t::post_ops_t &post_ops);
std::ostream &operator<<(std::ostream &s, const attr_t &attr);

/* Container for becnhdnn description of attributes and oneDNN primitive
 * attributes. Also contains the generated scales and zero-points.
 *
 * Usage model:
 * 1. Create attr_bundle_t with benchdnn attr
 * 2. Borrow and fill oscale
 *    - zero_point is automatically initialized at construct time
 *      (will be changed later)
 * 3. Call generate(scale_mask) to prepare dnnl_attr
 */
struct attr_bundle_t {
    attr_t attr;
    std::vector<float> oscale;
    std::map<int, std::vector<int>> zero_points; // arg -> arg_zero_points

    // constructor to forward already constructed oneDNN primitive attributes
    attr_bundle_t(const_dnnl_primitive_attr_t dnnl_attr)
        : dnnl_attr_((dnnl_primitive_attr_t)dnnl_attr,
                [](dnnl_primitive_attr_t) {}) {}

    attr_bundle_t(const attr_t &attr) : attr(attr) { init_zero_points(); }
    int generate(int scale_mask);

    const_dnnl_primitive_attr_t dnnl_attr() const { return dnnl_attr_.get(); }
    int scale_mask() const { return scale_mask_; }

private:
    bool initialized_ = false;
    int scale_mask_ = 0;
    std::shared_ptr<dnnl_primitive_attr> dnnl_attr_ {0};

    void init_zero_points();
};

struct engine_t {
    engine_t(dnnl_engine_kind_t engine_kind);
    ~engine_t();
    operator dnnl_engine_t() const { return engine_; }

private:
    BENCHDNN_DISALLOW_COPY_AND_ASSIGN(engine_t);
    dnnl_engine_t engine_;
};

struct stream_t {
    stream_t(dnnl_engine_t engine);
    ~stream_t();
    operator dnnl_stream_t() const { return stream_; }

private:
    BENCHDNN_DISALLOW_COPY_AND_ASSIGN(stream_t);
    dnnl_stream_t stream_;
};

std::ostream &dump_global_params(std::ostream &s);

dnnl_format_tag_t get_abx_tag(int ndims);
dnnl_format_tag_t get_axb_tag(int ndims);
dnnl_format_tag_t convert_tag(const std::string &tag_str, int ndims);

dnnl_primitive_attr_t create_dnnl_attr(const attr_t &attr, int64_t scale_cnt,
        int scale_mask, const float *scales);
inline dnnl_primitive_attr_t create_dnnl_attr(
        const attr_t &attr, int64_t scale_cnt, const float *scales) {
    return create_dnnl_attr(attr, scale_cnt, -1, scales);
}
inline dnnl_primitive_attr_t create_dnnl_attr(const attr_t &attr) {
    return create_dnnl_attr(attr, 1, -1, NULL);
}

dnnl_engine_kind_t str2engine_kind(const char *str);
dnnl_scratchpad_mode_t str2scratchpad_mode(const char *str);

void maybe_oscale(const attr_t &attr, float &d, float *scales, int64_t oc);
float compute_eltwise_fwd(attr_t::post_ops_t::kind_t kind, float src,
        float scale, float alpha, float beta);
float compute_eltwise_bwd(attr_t::post_ops_t::kind_t kind, float d_dst,
        float src, float alpha, float beta);
float compute_binary(attr_t::post_ops_t::kind_t kind, float src0, float src1);
void maybe_post_ops(const attr_t &attr, float &val, float sum_val = 0.f);

#endif
