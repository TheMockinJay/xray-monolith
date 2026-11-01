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
	MEMORY_BASIC_INFORMATION mbi;
	mbi.BaseAddress = 0;
	size_t w_free = 0, w_reserved = 0, w_committed = 0;
	size_t c_private = 0, c_mapped = 0, c_image = 0;
	size_t p_rw = 0, p_wc = 0, p_exec = 0, p_noacc = 0;
	size_t sz_u1m = 0, sz_1to16m = 0, sz_16to64m = 0, sz_o64m = 0;
	u32 cnt_o64m = 0;

	while (VirtualQuery(mbi.BaseAddress, &mbi, sizeof(mbi)))
	{
		const size_t rsz = mbi.RegionSize;
		switch (mbi.State)
		{
		case MEM_FREE:
			w_free += rsz;
			break;
		case MEM_RESERVE:
			w_reserved += rsz;
			break;
		case MEM_COMMIT:
			w_committed += rsz;
			if (mbi.Type == MEM_PRIVATE)
			{
				c_private += rsz;
				if (mbi.Protect & PAGE_WRITECOMBINE) p_wc += rsz;
				else if (mbi.Protect & PAGE_NOACCESS) p_noacc += rsz;
				else if (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) p_exec += rsz;
				else p_rw += rsz;

				if (rsz < 1024 * 1024) sz_u1m += rsz;
				else if (rsz < 16 * 1024 * 1024) sz_1to16m += rsz;
				else if (rsz < 64 * 1024 * 1024) sz_16to64m += rsz;
				else { sz_o64m += rsz; cnt_o64m++; Msg("* [win32 big_block #%u]: Base[0x%p] | AllocBase[0x%p] | Size[%llu MB] | Protect[0x%x]", cnt_o64m, mbi.BaseAddress, mbi.AllocationBase, rsz / (1024 * 1024), mbi.Protect); }
			}
			else if (mbi.Type == MEM_MAPPED) c_mapped += rsz;
			else if (mbi.Type == MEM_IMAGE) c_image += rsz;
			break;
		}
		mbi.BaseAddress = (char*)mbi.BaseAddress + rsz;
	}

	size_t win32_heap_alloc = 0, win32_heap_commit = 0;
	HANDLE heaps[64];
	DWORD num_heaps = GetProcessHeaps(64, heaps);
	if (num_heaps > 64) num_heaps = 64;
	for (DWORD i = 0; i < num_heaps; ++i)
	{
		HEAP_SUMMARY hs = {};
		hs.cb = sizeof(hs);
		if (HeapSummary(heaps[i], 0, &hs))
		{
			win32_heap_alloc += hs.cbAllocated;
			win32_heap_commit += hs.cbCommitted;
		}
	}

	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	pmc.cb = sizeof(pmc);
	K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
	const size_t tbb_live = g_xr_allocated_bytes.load(std::memory_order_relaxed);
	const s64 str_bytes = 0;
	const s64 smem_bytes = g_pSharedMemoryContainer ? g_pSharedMemoryContainer->stat_economy() : 0;

	Msg(
		"* [win32]: free[%llu MB], reserved[%llu MB], committed[%llu MB] (private[%llu MB], mapped_files[%llu MB], code_dll_image[%llu MB])",
		w_free / (1024 * 1024),
		w_reserved / (1024 * 1024),
		w_committed / (1024 * 1024),
		c_private / (1024 * 1024),
		c_mapped / (1024 * 1024),
		c_image / (1024 * 1024)
	);
	Msg(
		"* [win32]: working_set[%llu MB] (peak[%llu MB]) | private_bytes[%llu MB] | tbbmalloc_live[%llu MB] | win32_heaps[%u: alloc %llu MB / commit %llu MB]",
		pmc.WorkingSetSize / (1024 * 1024),
		pmc.PeakWorkingSetSize / (1024 * 1024),
		pmc.PrivateUsage / (1024 * 1024),
		tbb_live / (1024 * 1024),
		num_heaps,
		win32_heap_alloc / (1024 * 1024),
		win32_heap_commit / (1024 * 1024)
	);
	Msg(
		"* [win32]: private_protect(RW[%llu MB], GPU_WriteCombine[%llu MB], JIT_Exec[%llu MB], Guard/NoAcc[%llu MB]) | private_blocks(<1M[%llu MB], 1-16M[%llu MB], 16-64M[%llu MB], >=64M[%u : %llu MB]) | strings[%lld K] | smem_bones_shaders[%lld K]",
		p_rw / (1024 * 1024),
		p_wc / (1024 * 1024),
		p_exec / (1024 * 1024),
		p_noacc / (1024 * 1024),
		sz_u1m / (1024 * 1024),
		sz_1to16m / (1024 * 1024),
		sz_16to64m / (1024 * 1024),
		cnt_o64m,
		sz_o64m / (1024 * 1024),
		str_bytes / 1024,
		smem_bytes / 1024
	);
}

size_t xrMemory::mem_usage(bool assert)
{
	(void)assert;
	return g_xr_allocated_bytes.load(std::memory_order_relaxed);
}