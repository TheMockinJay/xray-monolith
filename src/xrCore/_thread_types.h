#pragma once

#include <tbb/task_group.h>
#include <tbb/task_arena.h>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>
#include <atomic>
#include <exception>
#include <functional>
#include <utility>
#include <algorithm>
#include <iterator>
#include <type_traits>

using xr_atomic_u32 = std::atomic_uint32_t;
using xr_atomic_s32 = std::atomic_int;
using xr_atomic_bool = std::atomic_bool;

void XRCORE_API Msg(const char* format, ...);

class xr_task_group
{
public:
    xr_task_group() = default;

    xr_task_group(const xr_task_group&) = delete;
    xr_task_group& operator=(const xr_task_group&) = delete;

    ~xr_task_group() noexcept
    {
        try
        {
            tbb::this_task_arena::isolate([this] {
                m_group.wait();
            });
        }
        catch (const std::exception& e)
        {
            Msg("! xr_task_group: exception escaped task_group::wait() during destruction: %s", e.what());
        }
        catch (...)
        {
            Msg("! xr_task_group: unknown exception escaped task_group::wait() during destruction");
        }
    }

    template <typename F>
    void run(F&& Functor)
    {
        m_group.run(std::forward<F>(Functor));
    }

    void wait()
    {
        tbb::this_task_arena::isolate([this] {
            m_group.wait();
        });
    }

private:
    tbb::task_group m_group;
};

using xr_background_task_group = xr_task_group;

template <typename T, typename U>
using xr_concurrent_unordered_map = tbb::concurrent_unordered_map<T, U>;

template <typename T, typename allocator = xalloc<T>>
using xr_concurrent_vector = tbb::concurrent_vector<T, allocator>;

template<typename BlockRangeType, typename Body>
inline void xr_parallel_for(BlockRangeType Begin, BlockRangeType End, Body Functor)
{
    tbb::this_task_arena::isolate([&] {
        tbb::parallel_for(Begin, End, Functor);
    });
}

template <typename Index, typename Index2, typename Body,
    typename = std::enable_if_t<std::is_integral_v<Index> && std::is_integral_v<Index2>>>
inline void xr_parallel_for(Index first, Index2 last, Body body)
{
    using Idx = std::common_type_t<Index, Index2>;
    if (static_cast<Idx>(last) <= static_cast<Idx>(first)) return;
    tbb::this_task_arena::isolate([&] {
        tbb::parallel_for(tbb::blocked_range<Idx>(static_cast<Idx>(first), static_cast<Idx>(last)),
            [&body](const tbb::blocked_range<Idx>& r)
            {
                for (Idx i = r.begin(); i != r.end(); ++i)
                    body(i);
            });
    });
}

template<typename Index, typename Body>
inline void xr_parallel_foreach(Index Begin, Index End, Body Functor)
{
    tbb::this_task_arena::isolate([&] {
        tbb::parallel_for_each(Begin, End, Functor);
    });
}

template <typename RandomIt>
using IterValueT = typename std::iterator_traits<RandomIt>::value_type;

template <typename It>
using IsRandomAccess = std::is_base_of<
    std::random_access_iterator_tag,
    typename std::iterator_traits<It>::iterator_category
>;

template<typename RandomIt, typename P = std::less<IterValueT<RandomIt>>>
IC void xr_parallel_sort(RandomIt first, RandomIt last, P pred = {})
{
    tbb::this_task_arena::isolate([&] {
        tbb::parallel_sort(first, last, pred);
    });
}