#include "stdafx.h"
#pragma hdrstop

#include "xrMemory_align.h"
#include "xrMemory_pure.h"
#include <tbb/scalable_allocator.h>
#include <atomic>
#pragma comment(lib, "tbbmalloc.lib")

std::atomic<size_t> g_xr_allocated_bytes{0};

#ifndef __BORLANDC__

#ifndef DEBUG_MEMORY_MANAGER
# define debug_mode 0
#endif

#ifdef DEBUG_MEMORY_MANAGER
XRCORE_API void* g_globalCheckAddr = NULL;
extern void save_stack_trace();
#endif

MEMPOOL mem_pools[mem_pools_count];

ICF u8* acc_header(void* P)
{
u8* _P = (u8*)P;
return _P - 1;
}

ICF u32 get_header(void* P) { return (u32)*acc_header(P); }
ICF u32 get_pool(size_t size)
{
u32 pid = u32(size / mem_pools_ebase);
if (pid >= mem_pools_count) return mem_generic;
else return pid;
}

#ifndef PURE_ALLOC
#define PURE_ALLOC
#endif
#define PURE_MEMORY_FILL_ZERO
#define PURE_MEMORY_ALIGNMENT (1 << 4)

void* xrMemory::mem_alloc(size_t size
# ifdef DEBUG_MEMORY_NAME
                          , const char* _name
# endif
, bool zeroMemory)
{
stat_calls++;

#ifdef PURE_ALLOC
{
const size_t alloc_size = size ? size : 1;
void* result = scalable_aligned_malloc(alloc_size, PURE_MEMORY_ALIGNMENT);
#ifdef PURE_MEMORY_FILL_ZERO
if (result)
{
const size_t usable = scalable_msize(result);
g_xr_allocated_bytes.fetch_add(usable, std::memory_order_relaxed);
if (zeroMemory)
memset(result, 0, usable >= alloc_size ? usable : alloc_size);
else if (usable > size)
memset(static_cast<u8*>(result) + size, 0, usable - size);
}
#endif

#ifdef USE_MEMORY_MONITOR
        memory_monitor::monitor_alloc(result, size, _name);
#endif
return (result);
}
#endif

#ifdef DEBUG_MEMORY_MANAGER
    if (mem_initialized) debug_cs.Enter();
#endif

u32 _footer = debug_mode ? 4 : 0;
void* _ptr = 0;

if (!mem_initialized)
{
void* _real = xr_aligned_offset_malloc(1 + size + _footer, 16, 0x1);
_ptr = (void*)(((u8*)_real) + 1);
*acc_header(_ptr) = mem_generic;
}
else
{
#ifdef DEBUG_MEMORY_MANAGER
        save_stack_trace();
#endif
u32 pool = get_pool(1 + size + _footer);
if (mem_generic == pool)
{
void* _real = xr_aligned_offset_malloc(1 + size + _footer, 16, 0x1);
_ptr = (void*)(((u8*)_real) + 1);
*acc_header(_ptr) = mem_generic;
}
else
{
void* _real = mem_pools[pool].create();
_ptr = (void*)(((u8*)_real) + 1);
*acc_header(_ptr) = (u8)pool;
}
}

#ifdef DEBUG_MEMORY_MANAGER
    if (debug_mode) dbg_register(_ptr, size, _name);
    if (mem_initialized) debug_cs.Leave();
#endif
#ifdef USE_MEMORY_MONITOR
    memory_monitor::monitor_alloc(_ptr, size, _name);
#endif
    if (zeroMemory)
    memset(_ptr, 0, size);
return _ptr;
}

void xrMemory::mem_free(void* P)
{
stat_calls++;
#ifdef USE_MEMORY_MONITOR
    memory_monitor::monitor_free(P);
#endif

#ifdef PURE_ALLOC
{
if (P)
{
const size_t usable = scalable_msize(P);
g_xr_allocated_bytes.fetch_sub(usable, std::memory_order_relaxed);
scalable_aligned_free(P);
}
return;
}
#endif

#ifdef DEBUG_MEMORY_MANAGER
    if (g_globalCheckAddr == P)
        __asm int 3;
#endif

#ifdef DEBUG_MEMORY_MANAGER
    if (mem_initialized) debug_cs.Enter();
#endif
if (debug_mode) dbg_unregister(P);
u32 pool = get_header(P);
void* _real = (void*)(((u8*)P) - 1);
if (mem_generic == pool)
{
xr_aligned_free(_real);
}
else
{
VERIFY2(pool < mem_pools_count, "Memory corruption");
mem_pools[pool].destroy(_real);
}
#ifdef DEBUG_MEMORY_MANAGER
    if (mem_initialized) debug_cs.Leave();
#endif
}

extern BOOL g_bDbgFillMemory;

void* xrMemory::mem_realloc(void* P, size_t size
#ifdef DEBUG_MEMORY_NAME
                            , const char* _name
#endif
)
{
stat_calls++;

if (0 == P)
{
return mem_alloc(size
# ifdef DEBUG_MEMORY_NAME
, _name
# endif
);
}

#ifdef PURE_ALLOC
{
const size_t alloc_size = size ? size : 1;
#ifdef PURE_MEMORY_FILL_ZERO
const size_t old_usable = scalable_msize(P);
#endif

void* result = scalable_aligned_realloc(P, alloc_size, PURE_MEMORY_ALIGNMENT);

#ifdef PURE_MEMORY_FILL_ZERO
if (result)
{
const size_t new_usable = scalable_msize(result);
if (new_usable >= old_usable)
g_xr_allocated_bytes.fetch_add(new_usable - old_usable, std::memory_order_relaxed);
else
g_xr_allocated_bytes.fetch_sub(old_usable - new_usable, std::memory_order_relaxed);
const size_t zero_from = size < old_usable ? size : old_usable;
if (new_usable > zero_from)
memset(static_cast<u8*>(result) + zero_from, 0, new_usable - zero_from);
}
#endif

# ifdef USE_MEMORY_MONITOR
        memory_monitor::monitor_free(P);
        memory_monitor::monitor_alloc(result, size, _name);
# endif
return (result);
}
#endif

#ifdef DEBUG_MEMORY_MANAGER
    if (g_globalCheckAddr == P)
        __asm int 3;
#endif

#ifdef DEBUG_MEMORY_MANAGER
    if (mem_initialized) debug_cs.Enter();
#endif
u32 p_current = get_header(P);
u32 p_new = get_pool(1 + size + (debug_mode ? 4 : 0));
u32 p_mode;

if (mem_generic == p_current)
{
if (p_new < p_current) p_mode = 2;
else p_mode = 0;
}
else p_mode = 1;

void* _real = (void*)(((u8*)P) - 1);
void* _ptr = NULL;
if (0 == p_mode)
{
u32 _footer = debug_mode ? 4 : 0;
#ifdef DEBUG_MEMORY_MANAGER
        if (debug_mode)
        {
            g_bDbgFillMemory = false;
            dbg_unregister(P);
            g_bDbgFillMemory = true;
        }
#endif
void* _real2 = xr_aligned_offset_realloc(_real, 1 + size + _footer, 16, 0x1);
_ptr = (void*)(((u8*)_real2) + 1);
*acc_header(_ptr) = mem_generic;
#ifdef DEBUG_MEMORY_MANAGER
        if (debug_mode) dbg_register(_ptr, size, _name);
#endif
#ifdef USE_MEMORY_MONITOR
        memory_monitor::monitor_free(P);
        memory_monitor::monitor_alloc(_ptr, size, _name);
#endif
}
else if (1 == p_mode)
{
R_ASSERT2(p_current < mem_pools_count, "Memory corruption");
u32 s_current = mem_pools[p_current].get_element();
u32 s_dest = (u32)size;
void* p_old = P;

void* p_new = mem_alloc(size
#ifdef DEBUG_MEMORY_NAME
                                , _name
#endif
);
mem_copy(p_new, p_old, _min(s_current - 1, s_dest));
mem_free(p_old);
_ptr = p_new;
}
else if (2 == p_mode)
{
void* p_old = P;
void* p_new = mem_alloc(size
# ifdef DEBUG_MEMORY_NAME
                                , _name
# endif
);
mem_copy(p_new, p_old, (u32)size);
mem_free(p_old);
_ptr = p_new;
}

#ifdef DEBUG_MEMORY_MANAGER
    if (mem_initialized) debug_cs.Leave();

    if (g_globalCheckAddr == _ptr)
        __asm int 3;
#endif

return _ptr;
}

#endif