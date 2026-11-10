// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include <boost/icl/interval.hpp>
#include <boost/icl/interval_base_set.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <boost/icl/split_interval_map.hpp>
#include <boost/pool/pool.hpp>
#include <boost/pool/poolfwd.hpp>

#include "common/nx2_early_switches.h"
#include "common/range_sets.h"

namespace Common {

// The nodes of each set come from a pool of its own (it was one fast_pool_allocator for the whole
// process, behind one std::mutex every set's node allocation and free took, from any thread).
// A set is used by one thread at a time (its owner's lock: the buffer cache's mutex, the heap
// mapper's), so its pool, used only with the set, needs no lock. The pool lives and dies with the
// set (declared before it in the Impl, freed after it); its memory goes back when the set does.
namespace RangeSetsDetail {
class NodePool {
public:
    void* Allocate(std::size_t bytes) {
        for (auto& slot : slots) {
            if (slot.bytes == 0) {
                slot.bytes = bytes;
                slot.pool.emplace(bytes, FirstChunks, MaxChunks);
            }
            if (slot.bytes == bytes) {
                if (void* const chunk = slot.pool->malloc()) {
                    return chunk;
                }
                throw std::bad_alloc{};
            }
        }
        return ::operator new(bytes);
    }

    void Deallocate(void* chunk, std::size_t bytes) noexcept {
        for (auto& slot : slots) {
            if (slot.bytes == bytes) {
                slot.pool->free(chunk);
                return;
            }
        }
        ::operator delete(chunk);
    }

private:
    // Chunks of the first block, and the most of a later one (a block doubles until then).
    static constexpr std::size_t FirstChunks = 32;
    static constexpr std::size_t MaxChunks = 2048;

    // The tree's nodes (and, in debug builds, the container's proxy): one size each.
    struct Slot {
        std::size_t bytes{};
        std::optional<boost::pool<boost::default_user_allocator_new_delete>> pool;
    };
    std::array<Slot, 2> slots{};
};

/// The pool a set made by MakeOnPool takes (its allocator is default constructed in the
/// container's constructor: Boost.ICL passes it no allocator).
inline thread_local NodePool* t_constructing{};

template <class T>
class Allocator {
public:
    using value_type = T;

    Allocator() noexcept : pool{t_constructing} {}
    template <class U>
    Allocator(const Allocator<U>& other) noexcept : pool{other.pool} {}

    T* allocate(std::size_t count) {
        if (count == 1 && pool != nullptr) {
            return static_cast<T*>(pool->Allocate(sizeof(T)));
        }
        return static_cast<T*>(::operator new(count * sizeof(T)));
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        if (count == 1 && pool != nullptr) {
            pool->Deallocate(pointer, sizeof(T));
        } else {
            ::operator delete(pointer);
        }
    }

    template <class U>
    bool operator==(const Allocator<U>& other) const noexcept {
        return pool == other.pool;
    }

    NodePool* pool;
};

/// A container whose allocator (and every allocator rebound from it) allocates from `pool`.
template <typename Container>
Container MakeOnPool(NodePool& pool) {
    struct Binding {
        NodePool* previous;
        ~Binding() {
            t_constructing = previous;
        }
    } const binding{std::exchange(t_constructing, &pool)};
    return Container{};
}
} // namespace RangeSetsDetail

namespace {
template <class T>
using RangeSetsAllocator = RangeSetsDetail::Allocator<T>;

/// EDEN_NX2_RANGE_SKIP=0: RangeSet::Add always inserts, as before. Read before main (not a
/// function-local static: Add runs several times per draw) and again by
/// Common::Nx2EarlySwitches::ReloadAll.
bool ReadRangeSkip() {
    const char* value = std::getenv("EDEN_NX2_RANGE_SKIP");
    return value == nullptr || value[0] != '0';
}
bool g_range_skip_covered_adds = ReadRangeSkip();
[[maybe_unused]] const bool g_range_skip_registered = Common::Nx2EarlySwitches::Register([] {
    g_range_skip_covered_adds = ReadRangeSkip();
});
}

template <typename AddressType>
struct RangeSet<AddressType>::RangeSetImpl {
    using IntervalSet = boost::icl::interval_set<
        AddressType, std::less, ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, AddressType, std::less),
        RangeSetsAllocator>;
    using IntervalType = typename IntervalSet::interval_type;

    RangeSetImpl() = default;
    ~RangeSetImpl() = default;

    void Add(AddressType base_address, size_t size) {
        AddressType end_address = base_address + static_cast<AddressType>(size);
        IntervalType interval{base_address, end_address};
        // EDEN_NX2_RANGE_SKIP: adding a range one stored range already covers leaves the set as
        // it is. The stored ranges are disjoint and joined, so the first one not entirely below
        // the range is the only one that can cover it. Each draw adds the ranges of its written
        // storage buffers again (~3.5% of the split back on the wide view in joins and frees).
        if (g_range_skip_covered_adds && size != 0 && !m_ranges_set.empty()) {
            const auto it = m_ranges_set.lower_bound(interval);
            if (it != m_ranges_set.end() && boost::icl::contains(*it, interval)) {
                return;
            }
        }
        m_ranges_set.add(interval);
    }

    void Subtract(AddressType base_address, size_t size) {
        AddressType end_address = base_address + static_cast<AddressType>(size);
        IntervalType interval{base_address, end_address};
        m_ranges_set.subtract(interval);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_ranges_set.empty()) {
            return;
        }
        auto it = m_ranges_set.begin();
        auto end_it = m_ranges_set.end();
        for (; it != end_it; it++) {
            const AddressType inter_addr_end = it->upper();
            const AddressType inter_addr = it->lower();
            func(inter_addr, inter_addr_end);
        }
    }

    template <typename Func>
    void ForEachInRange(AddressType base_addr, size_t size, Func&& func) const {
        if (m_ranges_set.empty()) {
            return;
        }
        const AddressType start_address = base_addr;
        const AddressType end_address = start_address + size;
        const RangeSetImpl::IntervalType search_interval{start_address, end_address};
        auto it = m_ranges_set.lower_bound(search_interval);
        if (it == m_ranges_set.end()) {
            return;
        }
        auto end_it = m_ranges_set.upper_bound(search_interval);
        for (; it != end_it; it++) {
            AddressType inter_addr_end = it->upper();
            AddressType inter_addr = it->lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end);
        }
    }

    RangeSetsDetail::NodePool m_pool; ///< Before the set: freed after it
    IntervalSet m_ranges_set = RangeSetsDetail::MakeOnPool<IntervalSet>(m_pool);
};

template <typename AddressType>
struct OverlapRangeSet<AddressType>::OverlapRangeSetImpl {
    using IntervalSet = boost::icl::split_interval_map<
        AddressType, s32, boost::icl::partial_enricher, std::less, boost::icl::inplace_plus,
        boost::icl::inter_section,
        ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, AddressType, std::less), RangeSetsAllocator>;
    using IntervalType = typename IntervalSet::interval_type;

    OverlapRangeSetImpl() = default;
    ~OverlapRangeSetImpl() = default;

    void Add(AddressType base_address, size_t size) {
        AddressType end_address = base_address + static_cast<AddressType>(size);
        IntervalType interval{base_address, end_address};
        m_split_ranges_set += std::make_pair(interval, 1);
    }

    template <bool has_on_delete, typename Func>
    void Subtract(AddressType base_address, size_t size, s32 amount,
                  [[maybe_unused]] Func&& on_delete) {
        if (m_split_ranges_set.empty()) {
            return;
        }
        AddressType end_address = base_address + static_cast<AddressType>(size);
        IntervalType interval{base_address, end_address};
        m_split_ranges_set += std::make_pair(interval, -amount);
        // One pass, erasing the segments that dropped to zero or below in address order (the
        // search restarted after every erase: quadratic in the segments of the range, and the
        // GPU fencing thread does this for each downloaded buffer range while holding the buffer
        // cache's lock). Erasing a node leaves the other iterators valid.
        auto it = m_split_ranges_set.lower_bound(interval);
        const auto end_it = m_split_ranges_set.upper_bound(interval);
        while (it != end_it) {
            if (it->second > 0) {
                ++it;
                continue;
            }
            if constexpr (has_on_delete) {
                if (it->second == 0) {
                    on_delete(it->first.lower(), it->first.upper());
                }
            }
            m_split_ranges_set.erase(it++);
        }
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_split_ranges_set.empty()) {
            return;
        }
        auto it = m_split_ranges_set.begin();
        auto end_it = m_split_ranges_set.end();
        for (; it != end_it; it++) {
            const AddressType inter_addr_end = it->first.upper();
            const AddressType inter_addr = it->first.lower();
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    template <typename Func>
    void ForEachInRange(AddressType base_address, size_t size, Func&& func) const {
        if (m_split_ranges_set.empty()) {
            return;
        }
        const AddressType start_address = base_address;
        const AddressType end_address = start_address + size;
        const OverlapRangeSetImpl::IntervalType search_interval{start_address, end_address};
        auto it = m_split_ranges_set.lower_bound(search_interval);
        if (it == m_split_ranges_set.end()) {
            return;
        }
        auto end_it = m_split_ranges_set.upper_bound(search_interval);
        for (; it != end_it; it++) {
            auto& inter = it->first;
            AddressType inter_addr_end = inter.upper();
            AddressType inter_addr = inter.lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    RangeSetsDetail::NodePool m_pool; ///< Before the set: freed after it
    IntervalSet m_split_ranges_set = RangeSetsDetail::MakeOnPool<IntervalSet>(m_pool);
};

template <typename AddressType>
RangeSet<AddressType>::RangeSet() {
    m_impl = std::make_unique<RangeSet<AddressType>::RangeSetImpl>();
}

template <typename AddressType>
RangeSet<AddressType>::~RangeSet() = default;

// A move takes the other's Impl whole (its nodes are its pool's) and leaves it an empty one, as
// moving the set left it empty.
template <typename AddressType>
RangeSet<AddressType>::RangeSet(RangeSet&& other) {
    m_impl = std::move(other.m_impl);
    other.m_impl = std::make_unique<RangeSet<AddressType>::RangeSetImpl>();
}

template <typename AddressType>
RangeSet<AddressType>& RangeSet<AddressType>::operator=(RangeSet&& other) {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
        other.m_impl = std::make_unique<RangeSet<AddressType>::RangeSetImpl>();
    }
    return *this;
}

template <typename AddressType>
void RangeSet<AddressType>::Add(AddressType base_address, size_t size) {
    m_impl->Add(base_address, size);
}

template <typename AddressType>
void RangeSet<AddressType>::Subtract(AddressType base_address, size_t size) {
    m_impl->Subtract(base_address, size);
}

template <typename AddressType>
void RangeSet<AddressType>::Clear() {
    m_impl->m_ranges_set.clear();
}

template <typename AddressType>
bool RangeSet<AddressType>::Empty() const {
    return m_impl->m_ranges_set.empty();
}

template <typename AddressType>
bool RangeSet<AddressType>::Intersects(AddressType base_address, size_t size) const {
    const auto& set = m_impl->m_ranges_set;
    if (set.empty() || size == 0) {
        return false;
    }
    const AddressType end_address = base_address + static_cast<AddressType>(size);
    // The first range not entirely below the query; it overlaps when it starts before its end.
    const auto it = set.lower_bound(typename RangeSetImpl::IntervalType{base_address, end_address});
    return it != set.end() && it->lower() < end_address;
}

template <typename AddressType>
template <typename Func>
void RangeSet<AddressType>::ForEach(Func&& func) const {
    m_impl->ForEach(std::move(func));
}

template <typename AddressType>
template <typename Func>
void RangeSet<AddressType>::ForEachInRange(AddressType base_address, size_t size,
                                           Func&& func) const {
    m_impl->ForEachInRange(base_address, size, std::move(func));
}

template <typename AddressType>
OverlapRangeSet<AddressType>::OverlapRangeSet() {
    m_impl = std::make_unique<OverlapRangeSet<AddressType>::OverlapRangeSetImpl>();
}

template <typename AddressType>
OverlapRangeSet<AddressType>::~OverlapRangeSet() = default;

template <typename AddressType>
OverlapRangeSet<AddressType>::OverlapRangeSet(OverlapRangeSet&& other) {
    m_impl = std::move(other.m_impl);
    other.m_impl = std::make_unique<OverlapRangeSet<AddressType>::OverlapRangeSetImpl>();
}

template <typename AddressType>
OverlapRangeSet<AddressType>& OverlapRangeSet<AddressType>::operator=(OverlapRangeSet&& other) {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
        other.m_impl = std::make_unique<OverlapRangeSet<AddressType>::OverlapRangeSetImpl>();
    }
    return *this;
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::Add(AddressType base_address, size_t size) {
    m_impl->Add(base_address, size);
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::Subtract(AddressType base_address, size_t size) {
    m_impl->template Subtract<false>(base_address, size, 1, [](AddressType, AddressType) {});
}

template <typename AddressType>
template <typename Func>
void OverlapRangeSet<AddressType>::Subtract(AddressType base_address, size_t size,
                                            Func&& on_delete) {
    m_impl->template Subtract<true, Func>(base_address, size, 1, std::move(on_delete));
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::DeleteAll(AddressType base_address, size_t size) {
    m_impl->template Subtract<false>(base_address, size, (std::numeric_limits<s32>::max)(),
                                     [](AddressType, AddressType) {});
}

template <typename AddressType>
void OverlapRangeSet<AddressType>::Clear() {
    m_impl->m_split_ranges_set.clear();
}

template <typename AddressType>
bool OverlapRangeSet<AddressType>::Empty() const {
    return m_impl->m_split_ranges_set.empty();
}

template <typename AddressType>
template <typename Func>
void OverlapRangeSet<AddressType>::ForEach(Func&& func) const {
    m_impl->ForEach(func);
}

template <typename AddressType>
template <typename Func>
void OverlapRangeSet<AddressType>::ForEachInRange(AddressType base_address, size_t size,
                                                  Func&& func) const {
    m_impl->ForEachInRange(base_address, size, std::move(func));
}

} // namespace Common
