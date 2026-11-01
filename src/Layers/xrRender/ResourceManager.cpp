
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "xrRender_console.h"
#pragma hdrstop
#include <psapi.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <atomic>
extern xr_task_group textures_load_tasks;
extern std::atomic<u32> g_native_dds_loaded;
extern std::atomic<u32> g_fallback_dds_loaded;

#pragma warning(disable:4995)
#include <d3dx9.h>
#pragma warning(default:4995)

#include "ResourceManager.h"
#include "tss.h"
#include "blenders\blender.h"
#include "blenders\blender_recorder.h"
#include "../../xrCore/_thread_types.h"

//	Already defined in Texture.cpp
void fix_texture_name(LPSTR fn);

/*
void fix_texture_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if(  _ext					&&
	  (0==stricmp(_ext,".tga")	||
		0==stricmp(_ext,".dds")	||
		0==stricmp(_ext,".bmp")	||
		0==stricmp(_ext,".ogm")	) )
		*_ext = 0;
}
*/
//--------------------------------------------------------------------------------------------------------------
template <class T>
BOOL reclaim(xr_vector<T*>& vec, const T* ptr)
{
	xr_vector<T*>::iterator it = vec.begin();
	xr_vector<T*>::iterator end = vec.end();
	for (; it != end; it++)
		if (*it == ptr)
		{
			vec.erase(it);
			return TRUE;
		}
	return FALSE;
}

//--------------------------------------------------------------------------------------------------------------
IBlender* CResourceManager::_GetBlender(LPCSTR Name)
{
	R_ASSERT(Name && Name[0]);

	LPSTR N = LPSTR(Name);
	map_Blender::iterator I = m_blenders.find(N);
#ifdef _EDITOR
	if (I==m_blenders.end())	return 0;
#else
	//	TODO: DX10: When all shaders are ready switch to common path
#if defined(USE_DX10) || defined(USE_DX11)
	if (I == m_blenders.end())
	{
		Msg("DX10: Shader '%s' not found in library.", Name);
		return 0;
	}
#endif
	if (I == m_blenders.end())
	{
		Debug.fatal(DEBUG_INFO, "Shader '%s' not found in library.", Name);
		return 0;
	}
#endif
	else return I->second;
}

IBlender* CResourceManager::_FindBlender(LPCSTR Name)
{
	if (!(Name && Name[0])) return 0;

	LPSTR N = LPSTR(Name);
	map_Blender::iterator I = m_blenders.find(N);
	if (I == m_blenders.end()) return 0;
	else return I->second;
}

void CResourceManager::ED_UpdateBlender(LPCSTR Name, IBlender* data)
{
	LPSTR N = LPSTR(Name);
	map_Blender::iterator I = m_blenders.find(N);
	if (I != m_blenders.end())
	{
		R_ASSERT(data->getDescription().CLS == I->second->getDescription().CLS);
		xr_delete(I->second);
		I->second = data;
	}
	else
	{
		m_blenders.insert(mk_pair(xr_strdup(Name), data));
	}
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CResourceManager::_ParseList(sh_list& dest, LPCSTR names)
{
	if (0 == names || 0 == names[0])
		names = "$null";

	ZeroMemory(&dest, sizeof(dest));
	char* P = (char*)names;
	svector<char, 128> N;

	while (*P)
	{
		if (*P == ',')
		{
			// flush
			N.push_back(0);
			strlwr(N.begin());

			fix_texture_name(N.begin());
			//. andy			if (strext(N.begin())) *strext(N.begin())=0;
			dest.push_back(N.begin());
			N.clear();
		}
		else
		{
			N.push_back(*P);
		}
		P++;
	}
	if (N.size())
	{
		// flush
		N.push_back(0);
		strlwr(N.begin());

		fix_texture_name(N.begin());
		//. andy		if (strext(N.begin())) *strext(N.begin())=0;
		dest.push_back(N.begin());
	}
}

ShaderElement* CResourceManager::_CreateElement(ShaderElement& S)
{
	if (S.passes.empty()) return 0;

	xrCriticalSectionGuard guard(creationGuard);

	// Search equal in shaders array
	for (u32 it = 0; it < v_elements.size(); it++)
		if (S.equal(*(v_elements[it]))) return v_elements[it];

	// Create _new_ entry
	ShaderElement* N = xr_new<ShaderElement>(S);
	//N->_copy(S);
	N->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_elements.push_back(N);
	return N;
}

void CResourceManager::_DeleteElement(const ShaderElement* S)
{
	xrCriticalSectionGuard guard(creationGuard);
	if (0 == (S->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	if (reclaim(v_elements, S)) return;
	Msg("! ERROR: Failed to find compiled 'shader-element'");
}

Shader* CResourceManager::_cpp_Create(IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants,
                                      LPCSTR s_matrices)
{
	xrCriticalSectionGuard guard(creationGuard);

	CBlender_Compile C;
	Shader S;

	//.
	// if (strstr(s_shader,"transparent"))	__asm int 3;

	// Access to template
	C.BT = B;
	C.bEditor = FALSE;
	C.bDetail = FALSE;

#if defined(USE_DX11)
	C.HudElement = false;
#endif

#ifdef _EDITOR
	if (!C.BT)			{ ELog.Msg(mtError,"Can't find shader '%s'",s_shader); return 0; }
	C.bEditor			= TRUE;
#endif

	// Parse names
	_ParseList(C.L_textures, s_textures);
	_ParseList(C.L_constants, s_constants);
	_ParseList(C.L_matrices, s_matrices);

#if defined(USE_DX11)
	if (::Render->hud_loading && RImplementation.o.ssfx_core)
	{
		C.HudElement = true;
	}
#endif

	// Compile element	(LOD0 - HQ)
	{
		C.iElement = 0;
		C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
		//.		C.bDetail			= _GetDetailTexture(*C.L_textures[0],C.detail_texture,C.detail_scaler);
		ShaderElement E;
		C._cpp_Compile(&E);
		S.E[0] = _CreateElement(E);
	}

	// Compile element	(LOD1)
	{
		C.iElement = 1;
		//.		C.bDetail			= _GetDetailTexture(*C.L_textures[0],C.detail_texture,C.detail_scaler);
		C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
		ShaderElement E;
		C._cpp_Compile(&E);
		S.E[1] = _CreateElement(E);
	}

	// Compile element
	{
		C.iElement = 2;
		C.bDetail = FALSE;
		ShaderElement E;
		C._cpp_Compile(&E);
		S.E[2] = _CreateElement(E);
	}

	// Compile element
	{
		C.iElement = 3;
		C.bDetail = FALSE;
		ShaderElement E;
		C._cpp_Compile(&E);
		S.E[3] = _CreateElement(E);
	}

	// Compile element
	{
		C.iElement = 4;
		C.bDetail = TRUE; //.$$$ HACK :)
		ShaderElement E;
		C._cpp_Compile(&E);
		S.E[4] = _CreateElement(E);
	}

	// Compile element
	{
		C.iElement = 5;
		C.bDetail = FALSE;
		ShaderElement E;
		C._cpp_Compile(&E);
		S.E[5] = _CreateElement(E);
	}

	// Hacky way to remove from the HUD mask transparent stuff. ( Let's try something better later... )
	if (::Render->hud_loading)
	{
		if (strstr(s_shader, "lens"))
			S.E[0]->passes[0]->ps->hud_disabled = TRUE;
	}

	// Search equal in shaders array
	for (u32 it = 0; it < v_shaders.size(); it++)
		if (S.equal(v_shaders[it])) return v_shaders[it];

	// Create _new_ entry
	Shader* ResultShader = _CreateShader(&S);
	return ResultShader;
}

Shader* CResourceManager::_cpp_Create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
	//#ifndef DEDICATED_SERVER
#ifndef _EDITOR
	if (!g_dedicated_server)
#endif
	{
		//	TODO: DX10: When all shaders are ready switch to common path
#if defined(USE_DX10) || defined(USE_DX11)
		IBlender* pBlender = _GetBlender(s_shader ? s_shader : "null");
		if (!pBlender) return NULL;
		return _cpp_Create(pBlender, s_shader, s_textures, s_constants, s_matrices);
#else	//	USE_DX10
		return _cpp_Create(_GetBlender(s_shader ? s_shader : "null"), s_shader, s_textures, s_constants, s_matrices);
#endif	//	USE_DX10
		//#else
	}
#ifndef _EDITOR
	else
#endif
	{
		return NULL;
	}
	//#endif
}

Shader* CResourceManager::Create(IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
	//#ifndef DEDICATED_SERVER
#ifndef _EDITOR
	if (!g_dedicated_server)
#endif
	{
		return _cpp_Create(B, s_shader, s_textures, s_constants, s_matrices);
		//#else
	}
#ifndef _EDITOR
	else
#endif
	{
		return NULL;
		//#endif
	}
}

Shader* CResourceManager::Create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
	//#ifndef DEDICATED_SERVER
#ifndef _EDITOR
	if (!g_dedicated_server)
#endif
	{
		//	TODO: DX10: When all shaders are ready switch to common path
#if defined(USE_DX10) || defined(USE_DX11)
		if (_lua_HasShader(s_shader))
			return _lua_Create(s_shader, s_textures);
		else
		{
			Shader* pShader = _cpp_Create(s_shader, s_textures, s_constants, s_matrices);
			if (pShader)
				return pShader;
			else
			{
				if (_lua_HasShader("stub_default"))
					return _lua_Create("stub_default", s_textures);
				else
				{
					FATAL("Can't find stub_default.s");
					return 0;
				}
			}
		}
#else	//	USE_DX10
#ifndef _EDITOR
		if (_lua_HasShader(s_shader))
			return _lua_Create(s_shader, s_textures);
		else
#endif
			return _cpp_Create(s_shader, s_textures, s_constants, s_matrices);
#endif	//	USE_DX10
	}
		//#else
#ifndef _EDITOR
	else
#endif
	{
		return NULL;
	}
	//#endif
}

void CResourceManager::Delete(const Shader* S)
{
	if (0 == (S->dwFlags & xr_resource_flagged::RF_REGISTERED))
		return;

	xrCriticalSectionGuard guard(creationGuard);

	if (reclaim(v_shaders, S))
		return;

	Msg("! ERROR: Failed to find complete shader");
}

void CResourceManager::DeferredUpload()
{
	if (!RDEVICE.b_is_Ready) return;

	textures_load_tasks.wait();

	PROCESS_MEMORY_COUNTERS_EX pmc_pre = {}, pmc_post = {}, pmc_trim = {}, pmc_comp = {};
	pmc_pre.cb = sizeof(pmc_pre);
	pmc_post.cb = sizeof(pmc_post);
	pmc_trim.cb = sizeof(pmc_trim);
	pmc_comp.cb = sizeof(pmc_comp);
	K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc_pre, sizeof(pmc_pre));

	g_native_dds_loaded.store(0, std::memory_order_relaxed);
	g_fallback_dds_loaded.store(0, std::memory_order_relaxed);

	const bool inLevel = (g_pGameLevel != nullptr);
	xr_vector<CTexture*> textures_to_load;
	u32 skipped_dormant = 0;
	{
		xrCriticalSectionGuard guard(creationGuard);
		textures_to_load.reserve(m_textures.size());
		for (auto& tex : m_textures)
		{
			CTexture* T = tex.second;
			if (!T || T->flags.bLoaded)
				continue;
			LPCSTR name = tex.first;
			const bool isCoreUI = (strncmp(name, "ui\\", 3) == 0) ||
			                      (strncmp(name, "ui/", 3) == 0) ||
			                      (strncmp(name, "ed\\", 3) == 0) ||
			                      (strstr(name, "$") != nullptr);
			if (!inLevel)
			{
				if (isCoreUI)
					textures_to_load.push_back(T);
				else
					skipped_dormant++;
			}
			else
			{
				if (strncmp(name, "sky\\", 4) == 0 || strncmp(name, "sky/", 4) == 0)
				{
					skipped_dormant++;
					continue;
				}
				const bool isSkyOrEnv = (strncmp(name, "fx\\", 3) == 0) ||
				                        (strncmp(name, "intro\\", 6) == 0);
				const bool isLevelActive = isCoreUI ||
				                           isSkyOrEnv ||
				                           (T->dwLastUsedFrame != 0);
				if (isLevelActive)
					textures_to_load.push_back(T);
				else
					skipped_dormant++;
			}
		}
	}

	tbb::task_arena upload_arena(6);
	upload_arena.execute([&] {
		xr_parallel_foreach(textures_to_load.begin(), textures_to_load.end(), [&](CTexture* texture)
		{
			texture->Load();
		});
	});
	textures_load_tasks.wait();

	K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc_post, sizeof(pmc_post));

#ifdef USE_DX11
	if (HW.pContext)
		HW.pContext->Flush();
	IDXGIDevice3* pDXGIDevice3 = nullptr;
	if (HW.pDevice && SUCCEEDED(HW.pDevice->QueryInterface(__uuidof(IDXGIDevice3), (void**)&pDXGIDevice3)) && pDXGIDevice3)
	{
		pDXGIDevice3->Trim();
		pDXGIDevice3->Release();
	}
#endif

	K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc_trim, sizeof(pmc_trim));
	Memory.mem_compact();
	K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc_comp, sizeof(pmc_comp));

	const s64 pre_mb  = s64(pmc_pre.PrivateUsage) / (1024 * 1024);
	const s64 post_mb = s64(pmc_post.PrivateUsage) / (1024 * 1024);
	const s64 trim_mb = s64(pmc_trim.PrivateUsage) / (1024 * 1024);
	const s64 comp_mb = s64(pmc_comp.PrivateUsage) / (1024 * 1024);
	const s64 ws_mb   = s64(pmc_comp.WorkingSetSize) / (1024 * 1024);

	Msg("* [DeferredUpload]: InLevel[%d] | Queued[%zu] | SkippedDormant[%u] | NativeZeroCopy[%u] | Fallback[%u] | Private: %lld MB -> %lld MB (Upload %+lld MB) -> DXGITrim: %lld MB (%+lld MB) -> Compact: %lld MB (%+lld MB) | WorkingSet: %lld MB",
		inLevel ? 1 : 0,
		textures_to_load.size(),
		skipped_dormant,
		g_native_dds_loaded.load(std::memory_order_relaxed),
		g_fallback_dds_loaded.load(std::memory_order_relaxed),
		pre_mb, post_mb, (post_mb - pre_mb), trim_mb, (trim_mb - post_mb), comp_mb, (comp_mb - trim_mb), ws_mb);
}

void CResourceManager::DeferredUnload()
{
	if (!RDEVICE.b_is_Ready)
		return;

	xr_vector<CTexture*> textures;
	{
		xrCriticalSectionGuard guard(creationGuard);
		textures.reserve(m_textures.size());
		for (auto& pair : m_textures)
			textures.push_back(pair.second);
	}

	xr_parallel_foreach(textures.begin(), textures.end(), [](CTexture* texture)
	{
		texture->Unload();
	});
}

void CResourceManager::UnloadAllTexturesOnLevelUnload()
{
	if (!RDEVICE.b_is_Ready)
		return;
	textures_load_tasks.wait();

    xrCriticalSectionGuard guard(creationGuard);

    xr_vector<CTexture*> textures_to_unload;
    textures_to_unload.reserve(m_textures.size());

    for (const auto& pair : m_textures)
    {
        CTexture* texture = pair.second;
        if (!texture)
            continue;

        if (texture->flags.bUser)
            continue;

        // Keep $ textures alive since they are bound to runtime render targets or other important parts
        if (strstr(*texture->cName, "$"))
            continue;

        // Keep UI textures
        LPCSTR name = pair.first;
        if (strncmp(name, "ui\\", 3) == 0 || strncmp(name, "ui/", 3) == 0)
            continue;

        textures_to_unload.push_back(texture);
    }

    for (CTexture* texture : textures_to_unload)
	{
		texture->dwLastUsedFrame = 0;
		texture->Unload();
	}
}

#ifdef _EDITOR
void	CResourceManager::ED_UpdateTextures(AStringVec* names)
{
	// 1. Unload
	if (names){
		for (u32 nid=0; nid<names->size(); nid++)
		{
			map_TextureIt I = m_textures.find	((*names)[nid].c_str());
			if (I!=m_textures.end())	I->second->Unload();
		}
	}else{
		for (map_TextureIt t=m_textures.begin(); t!=m_textures.end(); t++)
			t->second->Unload();
	}

	// 2. Load
	// DeferredUpload	();
}
#endif

Shader* CResourceManager::_CreateShader(Shader* InShader)
{
	xrCriticalSectionGuard guard(creationGuard);

	// Search equal in shaders array
	for (Shader* it : v_shaders)
	{
		if (InShader->equal(it))
			return it;
	}

	// Create _new_ entry
	Shader* N = xr_new<Shader>(*InShader);
	//N->_copy(*InShader);
	N->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_shaders.push_back(N);

	return N;
}

void CResourceManager::_GetMemoryUsage(u64& m_base, u32& c_base, u64& m_lmaps, u32& c_lmaps)
{
	m_base = c_base = m_lmaps = c_lmaps = 0;
	u64 m_sky = 0, m_ui = 0, m_pfx = 0, m_trees = 0, m_world = 0;
	u32 c_loaded = 0, c_dormant = 0;

	map_Texture::iterator I = m_textures.begin();
	map_Texture::iterator E = m_textures.end();
	for (; I != E; ++I)
	{
		LPCSTR name = I->first;
		CTexture* T = I->second;
		if (!T) continue;
		const u64 m = T->flags.bLoaded ? T->flags.MemoryUsage : 0;
		if (T->flags.bLoaded) c_loaded++; else c_dormant++;

		if (strstr(name, "lmap"))
		{
			c_lmaps++;
			m_lmaps += m;
		}
		else
		{
			c_base++;
			m_base += m;
			if (strncmp(name, "sky\\", 4) == 0 || strncmp(name, "sky/", 4) == 0)
				m_sky += m;
			else if (strncmp(name, "ui\\", 3) == 0 || strncmp(name, "ui/", 3) == 0 || strncmp(name, "intro\\", 6) == 0 || strncmp(name, "ed\\", 3) == 0)
				m_ui += m;
			else if (strncmp(name, "pfx\\", 4) == 0 || strncmp(name, "fx\\", 3) == 0 || strncmp(name, "semitone\\", 9) == 0 || strncmp(name, "jdude\\", 6) == 0 || strncmp(name, "wm\\", 3) == 0)
				m_pfx += m;
			else if (strncmp(name, "trees\\", 6) == 0 || strncmp(name, "lfo\\", 4) == 0 || strncmp(name, "detail\\", 7) == 0 || strncmp(name, "terrain\\", 8) == 0)
				m_trees += m;
			else
				m_world += m;
		}
	}

	u64 vram_local_mb = 0, vram_local_budget_mb = 0, vram_shared_mb = 0;
#ifdef USE_DX11
	if (HW.pContext)
		HW.pContext->Flush();
	IDXGIDevice3* pDXGIDevice3 = nullptr;
	if (HW.pDevice && SUCCEEDED(HW.pDevice->QueryInterface(__uuidof(IDXGIDevice3), (void**)&pDXGIDevice3)) && pDXGIDevice3)
	{
		pDXGIDevice3->Trim();
		pDXGIDevice3->Release();
	}
	IDXGIDevice* pDXGIDev = nullptr;
	if (HW.pDevice && SUCCEEDED(HW.pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDev)) && pDXGIDev)
	{
		IDXGIAdapter* pAdapter = nullptr;
		if (SUCCEEDED(pDXGIDev->GetAdapter(&pAdapter)) && pAdapter)
		{
			IDXGIAdapter3* pAdapter3 = nullptr;
			if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter3)) && pAdapter3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO infoLocal = {}, infoShared = {};
				if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &infoLocal)))
				{
					vram_local_mb = infoLocal.CurrentUsage / (1024 * 1024);
					vram_local_budget_mb = infoLocal.Budget / (1024 * 1024);
				}
				if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &infoShared)))
				{
					vram_shared_mb = infoShared.CurrentUsage / (1024 * 1024);
				}
				pAdapter3->Release();
			}
			pAdapter->Release();
		}
		pDXGIDev->Release();
	}
#endif

	Msg("* [ D3D 64-bit ]: loaded_tex[%u] | dormant_tex[%u] | total_true[%llu MB] (World/NPC/Wpn[%llu MB], Trees/Terrain[%llu MB], Particles/FX[%llu MB], Sky[%llu MB], UI[%llu MB], Lightmaps[%llu MB])",
		c_loaded, c_dormant,
		(m_base + m_lmaps) / (1024 * 1024),
		m_world / (1024 * 1024),
		m_trees / (1024 * 1024),
		m_pfx / (1024 * 1024),
		m_sky / (1024 * 1024),
		m_ui / (1024 * 1024),
		m_lmaps / (1024 * 1024)
	);
	Msg("* [ DXGI VRAM  ]: Dedicated_VRAM_Used[%llu MB / Budget %llu MB] | Shared_System_GPU_Used[%llu MB] | RTargets[%zu] | VS[%zu] | PS[%zu]",
		vram_local_mb, vram_local_budget_mb, vram_shared_mb,
		m_rtargets.size(), m_vs.size(), m_ps.size()
	);
}

void CResourceManager::_DumpMemoryUsage()
{
	xr_multimap<u32, std::pair<u32, shared_str>> mtex;

	// sort
	{
		map_Texture::iterator I = m_textures.begin();
		map_Texture::iterator E = m_textures.end();
		for (; I != E; I++)
		{
			u32 m = I->second->flags.MemoryUsage;
			shared_str n = I->second->cName;
			mtex.insert(mk_pair(m, mk_pair(I->second->dwReference.load(std::memory_order_relaxed), n)));
		}
	}

	// dump
	{
		xr_multimap<u32, std::pair<u32, shared_str>>::iterator I = mtex.begin();
		xr_multimap<u32, std::pair<u32, shared_str>>::iterator E = mtex.end();
		for (; I != E; I++)
			Msg("* %4.1f : [%4d] %s", float(I->first) / 1024.f, I->second.first, I->second.second.c_str());
	}
}

void CResourceManager::Evict()
{
	//	TODO: DX10: check if we really need this method
#if !defined(USE_DX10) && !defined(USE_DX11)
	CHK_DX(HW.pDevice->EvictManagedResources());
#endif	//	USE_DX10
}

void CResourceManager::EvictStalledTextures() {
#if defined(USE_DX11)
  if (!ps_r__tex_evict_enabled)
    return;
#ifdef DEBUG
  Msg("* [TexEvict] pass called, frame=%u", RDEVICE.dwFrame);
#endif // DEBUG
  if (!RDEVICE.b_is_Ready)
    return;

  u32 cur_frame = RDEVICE.dwFrame;
  u32 max_age   = (u32)ps_r__tex_evict_age_frames;
#ifdef DEBUG
  u32 evicted      = 0;
  u32 evicted_kb   = 0;
  u32 skip_user    = 0;
  u32 skip_name    = 0;
  u32 skip_unloaded = 0;
  u32 skip_refs    = 0;
  u32 skip_recent  = 0;
#endif // DEBUG

  static xr_string s_evict_cursor;
  const u32 batch_size = (u32)ps_r__tex_evict_batch_size;

  creationGuard.Enter();

  map_Texture::iterator I;
  if (s_evict_cursor.empty()) {
    I = m_textures.begin();
  } else {
    I = m_textures.lower_bound(s_evict_cursor.c_str());
    if (I == m_textures.end())
      I = m_textures.begin();
  }

  u32 scanned = 0;
  for (; I != m_textures.end() && scanned < batch_size; ++I, ++scanned) {
    CTexture *tex = I->second;
    if (tex->flags.bUser) {
#ifdef DEBUG
      skip_user++;
#endif // DEBUG
      continue;
    }
    if (tex->cName.size() && strstr(tex->cName.c_str(), "$user$")) {
#ifdef DEBUG
      skip_user++;
#endif // DEBUG
      continue;
    }
    if (tex->cName.size() && strstr(tex->cName.c_str(), "$null")) {
#ifdef DEBUG
      skip_user++;
#endif // DEBUG
      continue;
    }
    if (!tex->flags.bLoaded) {
#ifdef DEBUG
      skip_unloaded++;
#endif // DEBUG
      continue;
    }
    // UI textures — PDA, inventory icons, etc. — must never be evicted
    LPCSTR name = I->first;
    if (strncmp(name, "ui\\", 3) == 0 || strncmp(name, "ui/", 3) == 0) {
#ifdef DEBUG
      skip_name++;
#endif // DEBUG
      continue;
    }
    if (tex->dwReference.load(std::memory_order_relaxed) > 1) {
#ifdef DEBUG
      u32 refs = tex->dwReference.load(std::memory_order_relaxed);
      if (refs > 2)
        Msg("* [TexEvict] stuck: [%4d] %s", refs, tex->cName.c_str());
      skip_refs++;
#endif // DEBUG
      continue;
    }
    if (cur_frame - tex->dwLastUsedFrame < max_age) {
#ifdef DEBUG
      skip_recent++;
#endif // DEBUG
      continue;
    }

#ifdef DEBUG
    evicted_kb += tex->flags.MemoryUsage / 1024;
#endif // DEBUG
    tex->Unload();
#ifdef DEBUG
    evicted++;
#endif // DEBUG
  }

  s_evict_cursor = (I != m_textures.end()) ? xr_string(I->first) : xr_string();

  creationGuard.Leave();

#ifdef DEBUG
  Msg("* [TexEvict] frame=%u total=%u scanned=%u evicted=%u skip_user=%u "
      "skip_name=%u skip_unloaded=%u skip_refs=%u skip_recent=%u freed_kb=%u",
      cur_frame, (u32)m_textures.size(), scanned, evicted, skip_user, skip_name,
      skip_unloaded, skip_refs, skip_recent, evicted_kb);
#endif // DEBUG
#endif
}


/*
BOOL	CResourceManager::_GetDetailTexture(LPCSTR Name,LPCSTR& T, R_constant_setup* &CS)
{
	LPSTR N = LPSTR(Name);
	map_TD::iterator I = m_td.find	(N);
	if (I!=m_td.end())
	{
		T	= I->second.T;
		CS	= I->second.cs;
		return TRUE;
	} else {
		return FALSE;
	}
}*/
