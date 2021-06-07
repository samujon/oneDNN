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

#ifndef INTERFACE_PARTITION_CACHE_HPP
#define INTERFACE_PARTITION_CACHE_HPP

#include <future>
#include <memory>
#include <thread>
#include <unordered_map>

#include "oneapi/dnnl/dnnl_graph.h"

#include "c_types_map.hpp"
#include "partition_hashing.hpp"
#include "utils/rw_mutex.hpp"

namespace dnnl {
namespace graph {
namespace impl {

struct compiled_partition_cache_t {
    struct cache_value_t {
        std::shared_ptr<compiled_partition_t> compiled_partition;
        status_t status;
    };
    using key_t = partition_hashing::key_t;
    using value_t = std::shared_future<cache_value_t>;

    virtual ~compiled_partition_cache_t() = default;

    virtual status_t set_capacity(int capacity) = 0;
    virtual int get_capacity() const = 0;

    virtual value_t get_or_add(const key_t &key, const value_t &value) = 0;
    virtual void remove_if_invalidated(const key_t &key) = 0;
    virtual void update_entry(const key_t &key, const partition_t *partition,
            const std::vector<const logical_tensor_t *> &ins,
            const std::vector<const logical_tensor_t *> &outs)
            = 0;

    virtual int get_size() const = 0;

    virtual const partition_t *get_partition(const key_t &key) = 0;

protected:
    static utils::rw_mutex_t &rw_mutex() {
        static utils::rw_mutex_t mutex;
        return mutex;
    }

    void lock_read() { rw_mutex().lock_read(); }
    void lock_write() { rw_mutex().lock_write(); }
    void unlock_read() { rw_mutex().unlock_read(); }
    void unlock_write() { rw_mutex().unlock_write(); }
};

// The cache uses LRU replacement policy
struct lru_compiled_partition_cache_t : public compiled_partition_cache_t {
    lru_compiled_partition_cache_t(int capacity) : capacity_(capacity) {}

    ~lru_compiled_partition_cache_t() override = default;

    status_t set_capacity(int capacity) override;
    int get_capacity() const override;

    value_t get_or_add(const key_t &key, const value_t &value) override;
    void remove_if_invalidated(const key_t &key) override;
    void update_entry(const key_t &key, const partition_t *partition,
            const std::vector<const logical_tensor_t *> &ins,
            const std::vector<const logical_tensor_t *> &outs) override;

    int get_size() const override;

    const partition_t *get_partition(const key_t &key) override;

private:
    void evict(size_t n);
    void add(const key_t &key, const value_t &value);
    value_t get(const key_t &key);

    size_t capacity_;
    struct timed_entry_t {
        value_t value_;
        std::atomic<size_t> timestamp_;
        timed_entry_t(const value_t &value, size_t timestamp)
            : value_(value), timestamp_(timestamp) {}
    };
    // Each entry in the cache has a corresponding key and timestamp.
    // NOTE: pairs that contain atomics cannot be stored in an unordered_map *as
    // an element*, since it invokes the copy constructor of std::atomic, which
    // is deleted.
    std::unordered_map<key_t, timed_entry_t> cache_mapper_;
};

compiled_partition_cache_t &compiled_partition_cache();

// Undocumented API for testing
status_t DNNL_GRAPH_API get_compiled_partition_cache_size(int *size);
bool DNNL_GRAPH_API is_compiled_partition_in_cache(
        const compiled_partition_t *cp);
bool DNNL_GRAPH_API is_partition_in_cache(const partition_t *partition,
        const std::vector<const logical_tensor_t *> &ins,
        const std::vector<const logical_tensor_t *> &outs);

} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
