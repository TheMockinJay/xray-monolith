#include "stdafx.h"
#include <malloc.h>
#include <errno.h>
#include <atomic>
#include <psapi.h>

extern std::atomic<size_t> g_xr_allocated_bytes;

XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	MEMORY_BASIC_INFORMATION memory_info;
	memory_info.BaseAddress = 0;
	*_free = *reserved = *committed = 0;
	while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info)))
	{
		switch (memory_info.State)
		{
		case MEM_FREE:
			*_free += memory_info.RegionSize;
			break;
		case MEM_RESERVE:
			*reserved += memory_info.RegionSize;
			break;
		case MEM_COMMIT:
			*committed += memory_info.RegionSize;
			break;
		}
		memory_info.BaseAddress = (char*)memory_info.BaseAddress + memory_info.RegionSize;
	}
}

xrCriticalSection mem_lock;
XRCORE_API void log_vminfo()
{
	xrCriticalSectionGuard g(mem_lock);
	PROF_EVENT("log_vminfo");
	MEMORY_BASIC_INFORMATION memory_info;
	memory_info.BaseAddress = 0;
	size_t w_free = 0, w_reserved = 0, w_committed = 0;
	size_t c_private = 0, c_mapped = 0, c_image = 0;
	while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info)))
	{
		switch (memory_info.State)
		{
		case MEM_FREE:
			w_free += memory_info.RegionSize;
			break;
		case MEM_RESERVE:
			w_reserved += memory_info.RegionSize;
			break;
		case MEM_COMMIT:
			w_committed += memory_info.RegionSize;
			if (memory_info.Type == MEM_PRIVATE) c_private += memory_info.RegionSize;
			else if (memory_info.Type == MEM_MAPPED) c_mapped += memory_info.RegionSize;
			else if (memory_info.Type == MEM_IMAGE) c_image += memory_info.RegionSize;
			break;
		}
		memory_info.BaseAddress = (char*)memory_info.BaseAddress + memory_info.RegionSize;
	}
	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	pmc.cb = sizeof(pmc);
	K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
	const size_t tbb_live = g_xr_allocated_bytes.load(std::memory_order_relaxed);
	Msg(
		"* [win32]: free[%lld K], reserved[%lld K], committed[%lld K] (private[%lld K], mapped[%lld K], image[%lld K])",
		w_free / 1024,
		w_reserved / 1024,
		w_committed / 1024,
		c_private / 1024,
		c_mapped / 1024,
		c_image / 1024
	);
	Msg(
		"* [win32]: working_set[%lld K], private_bytes[%lld K], tbbmalloc_live[%lld K], non_tbb_private[%lld K]",
		pmc.WorkingSetSize / 1024,
		pmc.PrivateUsage / 1024,
		tbb_live / 1024,
		(pmc.PrivateUsage > tbb_live ? (pmc.PrivateUsage - tbb_live) : 0) / 1024
	);
}

size_t xrMemory::mem_usage(bool assert)
{
	(void)assert;
	return g_xr_allocated_bytes.load(std::memory_order_relaxed);
}
