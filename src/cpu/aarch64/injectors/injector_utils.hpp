/*******************************************************************************
* Copyright 2020 Intel Corporation
* Copyright 2020 FUJITSU LIMITED
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
#ifndef CPU_AARCH64_JIT_INJECTOR_UTILS_HPP
#define CPU_AARCH64_JIT_INJECTOR_UTILS_HPP

#include <array>
#include <cstddef>
#include <set>
#include <stack>

#include "cpu/aarch64/jit_generator.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace aarch64 {
namespace injector_utils {

using vmm_index_set_t = typename std::set<size_t>;
using vmm_index_set_iterator_t = typename std::set<size_t>::iterator;
template <typename Vmm>
struct vmm_size_t;

template <>
struct vmm_size_t<Xbyak_aarch64::ZReg> {
    static constexpr std::size_t bytes = 64u;
};

template <>
struct vmm_size_t<Xbyak_aarch64::VReg> {
    static constexpr std::size_t bytes = 16u;
};

/*
 * Scope guard for general purpouse register and vector registers preservation.
 * Pushes registers to stack during construction and pops during destruction.
 */
class register_preserve_guard_t {

public:
    register_preserve_guard_t(jit_generator *host,
            std::initializer_list<Xbyak_aarch64::XReg> reg64_to_preserve,
            std::initializer_list<Xbyak_aarch64::VReg> vmm_to_preserve = {});
    register_preserve_guard_t(register_preserve_guard_t &&other) = default;
    register_preserve_guard_t &operator=(register_preserve_guard_t &&other)
            = default;
    DNNL_DISALLOW_COPY_AND_ASSIGN(register_preserve_guard_t);
    ~register_preserve_guard_t();
    size_t stack_space_occupied() const;

private:
    jit_generator *host_;
    std::stack<Xbyak_aarch64::XReg> reg64_stack_;
    std::stack<Xbyak_aarch64::VReg> vmm_stack_;
    size_t vmm_to_preserve_size_bytes_;
};

using output_dims_t = std::array<dim_t, 5>;

output_dims_t make_output_dims(const memory_desc_wrapper &dst_d);

} // namespace injector_utils
} // namespace aarch64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif
