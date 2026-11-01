// Texture.cpp: implementation of the CTexture class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#pragma warning(disable:4995)
#include <d3dx9.h>
#pragma warning(default:4995)

#include <D3DX10Tex.h>

#include "../xrRender/dxRenderDeviceRender.h"

// #include "std_classes.h"
// #include "xr_avi.h"


#include <atomic>
#include <vector>
std::atomic<u32> g_native_dds_loaded{0};
std::atomic<u32> g_fallback_dds_loaded{0};
static xrCriticalSection s_d3d_upload_cs;
static size_t s_bytes_since_flush = 0;

void fix_texture_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if (_ext &&
		(0 == stricmp(_ext, ".tga") ||
			0 == stricmp(_ext, ".dds") ||
			0 == stricmp(_ext, ".bmp") ||
			0 == stricmp(_ext, ".ogm")))
		*_ext = 0;
}

int get_texture_load_lod(LPCSTR fn)
{
	if (psTextureLOD < 1)
		return 0;
	CInifile::Sect& sect = pSettings->r_section("reduce_lod_texture_list");
	CInifile::SectCIt it_ = sect.Data.begin();
	CInifile::SectCIt it_e_ = sect.Data.end();

	CInifile::SectCIt it = it_;
	CInifile::SectCIt it_e = it_e_;

	for (; it != it_e; ++it)
	{
		if (strstr(fn, it->first.c_str()))
		{
			if (psTextureLOD < 1)
				return 0;
			else if (psTextureLOD < 3)
				return 1;
			else
				return 2;
		}
	}

	if (psTextureLOD < 2)
		return 0;
	else if (psTextureLOD < 4)
		return 1;
	else
		return 2;
}

u32 calc_texture_size(int lod, u32 mip_cnt, u32 orig_size)
{
	if (1 == mip_cnt)
		return orig_size;

	int _lod = lod;
	float res = float(orig_size);

	while (_lod > 0)
	{
		--_lod;
		res -= res / 1.333f;
	}
	return iFloor(res);
}

const float _BUMPHEIGH = 8.f;
static const u32 c_dds_max_subresources = 128;

#pragma pack(push, 1)
struct XR_DDS_PIXELFORMAT
{
	u32 dwSize;
	u32 dwFlags;
	u32 dwFourCC;
	u32 dwRGBBitCount;
	u32 dwRBitMask;
	u32 dwGBitMask;
	u32 dwBBitMask;
	u32 dwABitMask;
};

struct XR_DDS_HEADER
{
	u32 dwSize;
	u32 dwFlags;
	u32 dwHeight;
	u32 dwWidth;
	u32 dwPitchOrLinearSize;
	u32 dwDepth;
	u32 dwMipMapCount;
	u32 dwReserved1[11];
	XR_DDS_PIXELFORMAT ddspf;
	u32 dwCaps;
	u32 dwCaps2;
	u32 dwCaps3;
	u32 dwCaps4;
	u32 dwReserved2;
};

struct XR_DDS_HEADER_DXT10
{
	u32 dxgiFormat;
	u32 resourceDimension;
	u32 miscFlag;
	u32 arraySize;
	u32 miscFlags2;
};
#pragma pack(pop)

enum XR_CONV_MODE
{
	CONV_NONE = 0,
	CONV_L8_TO_RGBA8,
	CONV_A8L8_TO_RGBA8,
	CONV_L16_TO_RGBA16,
	CONV_RGB24_TO_BGRX8,
	CONV_R5G6B5_TO_BGRA8,
	CONV_A1R5G5B5_TO_BGRA8,
	CONV_A4R4G4B4_TO_BGRA8
};

static bool GetDXGIFormatInfo(DXGI_FORMAT fmt, u32& outBlockBytes, u32& outBytesPerPixel, bool& outIsBC)
{
	outBlockBytes = 0;
	outBytesPerPixel = 0;
	outIsBC = false;

	switch (fmt)
	{
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		outBlockBytes = 8;
		outIsBC = true;
		return true;

	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		outBlockBytes = 16;
		outIsBC = true;
		return true;

	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		outBytesPerPixel = 16;
		return true;

	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
		outBytesPerPixel = 12;
		return true;

	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
		outBytesPerPixel = 8;
		return true;

	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
		outBytesPerPixel = 4;
		return true;

	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
		outBytesPerPixel = 2;
		return true;

	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
	case DXGI_FORMAT_A8_UNORM:
		outBytesPerPixel = 1;
		return true;

	default:
		return false;
	}
}

static bool MapDDSPixelFormat(const XR_DDS_PIXELFORMAT& pf, DXGI_FORMAT& outFmt, XR_CONV_MODE& outConv, u32& outSrcBpp)
{
	outFmt = DXGI_FORMAT_UNKNOWN;
	outConv = CONV_NONE;
	outSrcBpp = 0;

	const u32 DDPF_ALPHAPIXELS = 0x00000001;
	const u32 DDPF_ALPHA       = 0x00000002;
	const u32 DDPF_FOURCC      = 0x00000004;
	const u32 DDPF_RGB         = 0x00000040;
	const u32 DDPF_LUMINANCE   = 0x00020000;
	const u32 DDPF_BUMPDUDV    = 0x00080000;

	if (pf.dwFlags & DDPF_FOURCC)
	{
		switch (pf.dwFourCC)
		{
		case MAKEFOURCC('D','X','T','1'):
		case MAKEFOURCC('B','C','1',' '):
			outFmt = DXGI_FORMAT_BC1_UNORM;
			return true;
		case MAKEFOURCC('D','X','T','2'):
		case MAKEFOURCC('D','X','T','3'):
		case MAKEFOURCC('B','C','2',' '):
			outFmt = DXGI_FORMAT_BC2_UNORM;
			return true;
		case MAKEFOURCC('D','X','T','4'):
		case MAKEFOURCC('D','X','T','5'):
		case MAKEFOURCC('B','C','3',' '):
			outFmt = DXGI_FORMAT_BC3_UNORM;
			return true;
		case MAKEFOURCC('A','T','I','1'):
		case MAKEFOURCC('B','C','4','U'):
			outFmt = DXGI_FORMAT_BC4_UNORM;
			return true;
		case MAKEFOURCC('B','C','4','S'):
			outFmt = DXGI_FORMAT_BC4_SNORM;
			return true;
		case MAKEFOURCC('A','T','I','2'):
		case MAKEFOURCC('B','C','5','U'):
		case MAKEFOURCC('3','D','C',' '):
			outFmt = DXGI_FORMAT_BC5_UNORM;
			return true;
		case MAKEFOURCC('B','C','5','S'):
			outFmt = DXGI_FORMAT_BC5_SNORM;
			return true;
		case 36:
			outFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
			return true;
		case 110:
			outFmt = DXGI_FORMAT_R16G16B16A16_SNORM;
			return true;
		case 111:
			outFmt = DXGI_FORMAT_R16_FLOAT;
			return true;
		case 112:
			outFmt = DXGI_FORMAT_R16G16_FLOAT;
			return true;
		case 113:
			outFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
			return true;
		case 114:
			outFmt = DXGI_FORMAT_R32_FLOAT;
			return true;
		case 115:
			outFmt = DXGI_FORMAT_R32G32_FLOAT;
			return true;
		case 116:
			outFmt = DXGI_FORMAT_R32G32B32A32_FLOAT;
			return true;
		default:
			return false;
		}
	}

	if (pf.dwFlags & DDPF_RGB)
	{
		switch (pf.dwRGBBitCount)
		{
		case 32:
			if (pf.dwRBitMask == 0x00ff0000 && pf.dwGBitMask == 0x0000ff00 && pf.dwBBitMask == 0x000000ff)
			{
				outFmt = (pf.dwFlags & DDPF_ALPHAPIXELS) || pf.dwABitMask ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
				return true;
			}
			if (pf.dwRBitMask == 0x000000ff && pf.dwGBitMask == 0x0000ff00 && pf.dwBBitMask == 0x00ff0000)
			{
				outFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
				return true;
			}
			if (pf.dwRBitMask == 0x0000ffff && pf.dwGBitMask == 0xffff0000 && pf.dwBBitMask == 0)
			{
				outFmt = DXGI_FORMAT_R16G16_UNORM;
				return true;
			}
			if (pf.dwRBitMask == 0x3ff00000 && pf.dwGBitMask == 0x000ffc00 && pf.dwBBitMask == 0x000003ff)
			{
				outFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
				return true;
			}
			break;

		case 24:
			outFmt = DXGI_FORMAT_B8G8R8X8_UNORM;
			outConv = CONV_RGB24_TO_BGRX8;
			outSrcBpp = 3;
			return true;

		case 16:
			if (pf.dwRBitMask == 0xf800 && pf.dwGBitMask == 0x07e0 && pf.dwBBitMask == 0x001f)
			{
				outFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
				outConv = CONV_R5G6B5_TO_BGRA8;
				outSrcBpp = 2;
				return true;
			}
			if (pf.dwRBitMask == 0x7c00 && pf.dwGBitMask == 0x03e0 && pf.dwBBitMask == 0x001f)
			{
				outFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
				outConv = CONV_A1R5G5B5_TO_BGRA8;
				outSrcBpp = 2;
				return true;
			}
			if (pf.dwRBitMask == 0x0f00 && pf.dwGBitMask == 0x00f0 && pf.dwBBitMask == 0x000f)
			{
				outFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
				outConv = CONV_A4R4G4B4_TO_BGRA8;
				outSrcBpp = 2;
				return true;
			}
			break;
		}
	}

	if (pf.dwFlags & DDPF_LUMINANCE)
	{
		if (pf.dwRGBBitCount == 8)
		{
			outFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			outConv = CONV_L8_TO_RGBA8;
			outSrcBpp = 1;
			return true;
		}
		if (pf.dwRGBBitCount == 16)
		{
			if ((pf.dwFlags & DDPF_ALPHAPIXELS) || pf.dwABitMask == 0xff00)
			{
				outFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
				outConv = CONV_A8L8_TO_RGBA8;
				outSrcBpp = 2;
				return true;
			}
			outFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
			outConv = CONV_L16_TO_RGBA16;
			outSrcBpp = 2;
			return true;
		}
	}

	if ((pf.dwFlags & DDPF_ALPHA) && pf.dwRGBBitCount == 8)
	{
		outFmt = DXGI_FORMAT_A8_UNORM;
		return true;
	}

	if (pf.dwFlags & DDPF_BUMPDUDV)
	{
		if (pf.dwRGBBitCount == 16)
		{
			outFmt = DXGI_FORMAT_R8G8_SNORM;
			return true;
		}
		if (pf.dwRGBBitCount == 32)
		{
			outFmt = DXGI_FORMAT_R8G8B8A8_SNORM;
			return true;
		}
	}

	return false;
}

static ID3DBaseTexture* NativeCreateTextureFromDDS(
	const u8* fileData,
	u32 fileLen,
	bool bStaging,
	LPCSTR fn,
	u32& outMemSize,
	int& outLoadedLod)
{
	if (!fileData || fileLen < 4 + sizeof(XR_DDS_HEADER))
		return nullptr;

	const u32 magic = *reinterpret_cast<const u32*>(fileData);
	if (magic != 0x20534444)
		return nullptr;

	const XR_DDS_HEADER* hdr = reinterpret_cast<const XR_DDS_HEADER*>(fileData + 4);
	if (hdr->dwSize != sizeof(XR_DDS_HEADER) || hdr->ddspf.dwSize != sizeof(XR_DDS_PIXELFORMAT))
		return nullptr;

	u32 offset = 4 + sizeof(XR_DDS_HEADER);
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	XR_CONV_MODE convMode = CONV_NONE;
	u32 convSrcBpp = 0;

	const u32 DDSD_MIPMAPCOUNT = 0x00020000;
	const u32 DDSD_DEPTH       = 0x00800000;
	const u32 DDSCAPS2_CUBEMAP = 0x00000200;
	const u32 DDSCAPS2_VOLUME  = 0x00200000;

	u32 width    = hdr->dwWidth ? hdr->dwWidth : 1;
	u32 height   = hdr->dwHeight ? hdr->dwHeight : 1;
	u32 depth    = ((hdr->dwFlags & DDSD_DEPTH) || (hdr->dwCaps2 & DDSCAPS2_VOLUME)) && hdr->dwDepth > 1 ? hdr->dwDepth : 1;
	u32 mipCount = ((hdr->dwFlags & DDSD_MIPMAPCOUNT) && hdr->dwMipMapCount > 0) ? hdr->dwMipMapCount : 1;
	bool isCube  = (hdr->dwCaps2 & DDSCAPS2_CUBEMAP) != 0;
	bool is3D    = (depth > 1) && !isCube;
	u32 arraySize = isCube ? 6 : 1;

	if ((hdr->ddspf.dwFlags & 0x00000004) && hdr->ddspf.dwFourCC == MAKEFOURCC('D','X','1','0'))
	{
		if (fileLen < offset + sizeof(XR_DDS_HEADER_DXT10))
			return nullptr;
		const XR_DDS_HEADER_DXT10* dxt10 = reinterpret_cast<const XR_DDS_HEADER_DXT10*>(fileData + offset);
		offset += sizeof(XR_DDS_HEADER_DXT10);
		fmt = static_cast<DXGI_FORMAT>(dxt10->dxgiFormat);
		if (dxt10->resourceDimension == 4)
			is3D = true;
		if (dxt10->miscFlag & 0x4)
		{
			isCube = true;
			arraySize = (dxt10->arraySize > 0 ? dxt10->arraySize : 1) * 6;
		}
		else if (dxt10->arraySize > 1 && !is3D)
		{
			arraySize = dxt10->arraySize;
		}
	}
	else
	{
		if (!MapDDSPixelFormat(hdr->ddspf, fmt, convMode, convSrcBpp))
			return nullptr;
	}

	u32 blockBytes = 0, bytesPerPixel = 0;
	bool isBC = false;
	if (!GetDXGIFormatInfo(fmt, blockBytes, bytesPerPixel, isBC))
		return nullptr;

	if (arraySize * mipCount > c_dds_max_subresources)
		return nullptr;

	D3D_SUBRESOURCE_DATA subres[c_dds_max_subresources];
	std::vector<u8> convertedBuffer;

	if (convMode != CONV_NONE)
	{
		u32 totalPixels = 0;
		u32 availMips = 0;
		u32 scanOff = offset;
		for (u32 a = 0; a < arraySize; ++a)
		{
			u32 mw = width, mh = height, md = depth;
			for (u32 m = 0; m < mipCount; ++m)
			{
				const u32 slicePixels = mw * mh;
				const u32 mipBytes = slicePixels * md * convSrcBpp;
				if (scanOff + mipBytes > fileLen)
				{
					if (a == 0 && m > 0)
						mipCount = m;
					else
						return nullptr;
					break;
				}
				scanOff += mipBytes;
				totalPixels += slicePixels * md;
				if (a == 0) availMips++;
				mw = mw > 1 ? (mw >> 1) : 1;
				mh = mh > 1 ? (mh >> 1) : 1;
				md = md > 1 ? (md >> 1) : 1;
			}
		}

		convertedBuffer.resize(size_t(totalPixels) * bytesPerPixel);
		u8* dstBase = convertedBuffer.data();
		u32 srcOff = offset;
		u32 dstOff = 0;

		for (u32 a = 0; a < arraySize; ++a)
		{
			u32 mw = width, mh = height, md = depth;
			for (u32 m = 0; m < mipCount; ++m)
			{
				const u32 idx = a * mipCount + m;
				const u32 count = mw * mh * md;
				const u8* src = fileData + srcOff;
				u8* dst = dstBase + dstOff;

				if (convMode == CONV_L8_TO_RGBA8)
				{
					u32* d32 = reinterpret_cast<u32*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u32 L = src[i];
						d32[i] = 0xFF000000u | (L << 16) | (L << 8) | L;
					}
				}
				else if (convMode == CONV_A8L8_TO_RGBA8)
				{
					u32* d32 = reinterpret_cast<u32*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u32 L = src[i * 2 + 0];
						const u32 A = src[i * 2 + 1];
						d32[i] = (A << 24) | (L << 16) | (L << 8) | L;
					}
				}
				else if (convMode == CONV_L16_TO_RGBA16)
				{
					const u16* s16 = reinterpret_cast<const u16*>(src);
					u16* d16 = reinterpret_cast<u16*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u16 L = s16[i];
						d16[i * 4 + 0] = L;
						d16[i * 4 + 1] = L;
						d16[i * 4 + 2] = L;
						d16[i * 4 + 3] = 0xFFFFu;
					}
				}
				else if (convMode == CONV_RGB24_TO_BGRX8)
				{
					u32* d32 = reinterpret_cast<u32*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u32 B = src[i * 3 + 0];
						const u32 G = src[i * 3 + 1];
						const u32 R = src[i * 3 + 2];
						d32[i] = 0xFF000000u | (R << 16) | (G << 8) | B;
					}
				}
				else if (convMode == CONV_R5G6B5_TO_BGRA8)
				{
					const u16* s16 = reinterpret_cast<const u16*>(src);
					u32* d32 = reinterpret_cast<u32*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u16 v = s16[i];
						const u32 R = ((v >> 11) & 31u) * 255u / 31u;
						const u32 G = ((v >> 5) & 63u) * 255u / 63u;
						const u32 B = (v & 31u) * 255u / 31u;
						d32[i] = 0xFF000000u | (R << 16) | (G << 8) | B;
					}
				}
				else if (convMode == CONV_A1R5G5B5_TO_BGRA8)
				{
					const u16* s16 = reinterpret_cast<const u16*>(src);
					u32* d32 = reinterpret_cast<u32*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u16 v = s16[i];
						const u32 A = (v & 0x8000u) ? 255u : 0u;
						const u32 R = ((v >> 10) & 31u) * 255u / 31u;
						const u32 G = ((v >> 5) & 31u) * 255u / 31u;
						const u32 B = (v & 31u) * 255u / 31u;
						d32[i] = (A << 24) | (R << 16) | (G << 8) | B;
					}
				}
				else if (convMode == CONV_A4R4G4B4_TO_BGRA8)
				{
					const u16* s16 = reinterpret_cast<const u16*>(src);
					u32* d32 = reinterpret_cast<u32*>(dst);
					for (u32 i = 0; i < count; ++i)
					{
						const u16 v = s16[i];
						const u32 A = ((v >> 12) & 15u) * 17u;
						const u32 R = ((v >> 8) & 15u) * 17u;
						const u32 G = ((v >> 4) & 15u) * 17u;
						const u32 B = (v & 15u) * 17u;
						d32[i] = (A << 24) | (R << 16) | (G << 8) | B;
					}
				}

				subres[idx].pSysMem = dst;
				subres[idx].SysMemPitch = mw * bytesPerPixel;
				subres[idx].SysMemSlicePitch = mw * mh * bytesPerPixel;

				srcOff += count * convSrcBpp;
				dstOff += count * bytesPerPixel;
				mw = mw > 1 ? (mw >> 1) : 1;
				mh = mh > 1 ? (mh >> 1) : 1;
				md = md > 1 ? (md >> 1) : 1;
			}
		}
	}
	else
	{
		u32 curOff = offset;
		for (u32 a = 0; a < arraySize; ++a)
		{
			u32 mw = width, mh = height, md = depth;
			for (u32 m = 0; m < mipCount; ++m)
			{
				u32 rowBytes = 0, sliceBytes = 0;
				if (isBC)
				{
					const u32 bw = _max(1u, (mw + 3u) / 4u);
					const u32 bh = _max(1u, (mh + 3u) / 4u);
					rowBytes = bw * blockBytes;
					sliceBytes = rowBytes * bh;
				}
				else
				{
					rowBytes = mw * bytesPerPixel;
					sliceBytes = rowBytes * mh;
				}
				const u32 mipBytes = sliceBytes * md;
				if (curOff + mipBytes > fileLen)
				{
					if (a == 0 && m > 0)
						mipCount = m;
					else
						return nullptr;
					break;
				}
				const u32 idx = a * mipCount + m;
				subres[idx].pSysMem = fileData + curOff;
				subres[idx].SysMemPitch = rowBytes;
				subres[idx].SysMemSlicePitch = sliceBytes;
				curOff += mipBytes;

				mw = mw > 1 ? (mw >> 1) : 1;
				mh = mh > 1 ? (mh >> 1) : 1;
				md = md > 1 ? (md >> 1) : 1;
			}
		}
	}

	int mip_lod = 0;
	if (!bStaging && !isCube && !is3D && arraySize == 1 && mipCount > 1)
	{
		mip_lod = get_texture_load_lod(fn);
		if (mip_lod >= int(mipCount))
			mip_lod = int(mipCount) - 1;
		while (mip_lod > 0)
		{
			const u32 lw = _max(1u, width >> mip_lod);
			const u32 lh = _max(1u, height >> mip_lod);
			if (isBC && (lw < 4 || lh < 4 || (lw & 3u) != 0 || (lh & 3u) != 0))
				--mip_lod;
			else
				break;
		}
	}

	if (is3D)
	{
		D3D_TEXTURE3D_DESC desc3D = {};
		desc3D.Width = _max(1u, width >> mip_lod);
		desc3D.Height = _max(1u, height >> mip_lod);
		desc3D.Depth = _max(1u, depth >> mip_lod);
		desc3D.MipLevels = mipCount - mip_lod;
		desc3D.Format = fmt;
		desc3D.Usage = bStaging ? D3D_USAGE_STAGING : D3D_USAGE_IMMUTABLE;
		desc3D.BindFlags = bStaging ? 0 : D3D_BIND_SHADER_RESOURCE;
		desc3D.CPUAccessFlags = bStaging ? D3D_CPU_ACCESS_WRITE : 0;
		desc3D.MiscFlags = 0;

		ID3DTexture3D* pTex3D = nullptr;
		HRESULT hr = HW.pDevice->CreateTexture3D(&desc3D, &subres[mip_lod], &pTex3D);
		if (SUCCEEDED(hr) && pTex3D)
		{
			outLoadedLod = mip_lod;
			outMemSize = calc_texture_size(mip_lod, mipCount, fileLen);
			g_native_dds_loaded.fetch_add(1, std::memory_order_relaxed);
			return pTex3D;
		}
		return nullptr;
	}
	else
	{
		u32 baseW = _max(1u, width >> mip_lod);
		u32 baseH = _max(1u, height >> mip_lod);
		u32 useMips = mipCount - mip_lod;
		if (isBC)
		{
			if (baseW < 4 || baseH < 4 || (baseW & 3u) != 0 || (baseH & 3u) != 0)
			{
				baseW = _max(4u, (baseW + 3u) & ~3u);
				baseH = _max(4u, (baseH + 3u) & ~3u);
				if (!isCube && arraySize == 1)
					useMips = 1;
			}
		}

		D3D_TEXTURE2D_DESC desc2D = {};
		desc2D.Width = baseW;
		desc2D.Height = baseH;
		desc2D.MipLevels = useMips;
		desc2D.ArraySize = arraySize;
		desc2D.Format = fmt;
		desc2D.SampleDesc.Count = 1;
		desc2D.SampleDesc.Quality = 0;
		desc2D.Usage = bStaging ? D3D_USAGE_STAGING : D3D_USAGE_IMMUTABLE;
		desc2D.BindFlags = bStaging ? 0 : D3D_BIND_SHADER_RESOURCE;
		desc2D.CPUAccessFlags = bStaging ? D3D_CPU_ACCESS_WRITE : 0;
		desc2D.MiscFlags = isCube ? D3D_RESOURCE_MISC_TEXTURECUBE : 0;

		ID3DTexture2D* pTex2D = nullptr;
		HRESULT hr = HW.pDevice->CreateTexture2D(&desc2D, &subres[mip_lod], &pTex2D);
		if (FAILED(hr) && useMips > 1 && !isCube && arraySize == 1)
		{
			desc2D.MipLevels = 1;
			hr = HW.pDevice->CreateTexture2D(&desc2D, &subres[mip_lod], &pTex2D);
		}
		if (SUCCEEDED(hr) && pTex2D)
		{
			outLoadedLod = mip_lod;
			outMemSize = calc_texture_size(mip_lod, desc2D.MipLevels, fileLen);
			g_native_dds_loaded.fetch_add(1, std::memory_order_relaxed);
			return pTex2D;
		}
		return nullptr;
	}
}

template <class _It>
IC void TW_Iterate_1OP
(
	ID3DTexture2D* t_dst,
	ID3DTexture2D* t_src,
	const _It pred
)
{
	D3D_TEXTURE2D_DESC desc;
	t_dst->GetDesc(&desc);
	VERIFY(desc.MipLevels == t_src->GetLevelCount());
	for (u32 i = 0; i < desc.MipLevels; i++)
	{
		D3D_MAPPED_TEXTURE2D map_src, map_dst;
		u32 d_sub = D3D10CalcSubresource(i, 0, desc.MipLevels);
#ifdef USE_DX11
		CHK_DX(HW.pContext->Map(t_src, d_sub, D3D_MAP_READ, 0, &map_src));
		CHK_DX(HW.pContext->Map(t_dst, d_sub, D3D_MAP_WRITE, 0, &map_dst));
#else
		CHK_DX(t_src->Map(d_sub, D3D_MAP_READ, 0, &map_src));
		CHK_DX(t_dst->Map(d_sub, D3D_MAP_WRITE, 0, &map_dst));
#endif
		u32 m_w = desc.Width >> i;
		u32 m_h = desc.Height >> i;
		if (m_w == 0) m_w = 1;
		if (m_h == 0) m_h = 1;
		pred(m_w, m_h, map_dst.RowPitch, map_src.RowPitch, (u8*)map_dst.pData, (u8*)map_src.pData);
#ifdef USE_DX11
		HW.pContext->Unmap(t_src, d_sub);
		HW.pContext->Unmap(t_dst, d_sub);
#else
		t_src->Unmap(d_sub);
		t_dst->Unmap(d_sub);
#endif
	}
}

template <class _It>
IC void TW_Iterate_2OP
(
	ID3DTexture2D* t_dst,
	ID3DTexture2D* t_src0,
	ID3DTexture2D* t_src1,
	const _It pred
)
{
	D3D_TEXTURE2D_DESC desc;
	t_dst->GetDesc(&desc);
	VERIFY(desc.MipLevels == t_src0->GetLevelCount());
	VERIFY(desc.MipLevels == t_src1->GetLevelCount());
	for (u32 i = 0; i < desc.MipLevels; i++)
	{
		D3D_MAPPED_TEXTURE2D map_src0, map_src1, map_dst;
		u32 d_sub = D3D10CalcSubresource(i, 0, desc.MipLevels);
#ifdef USE_DX11
		CHK_DX(HW.pContext->Map(t_src0, d_sub, D3D_MAP_READ, 0, &map_src0));
		CHK_DX(HW.pContext->Map(t_src1, d_sub, D3D_MAP_READ, 0, &map_src1));
		CHK_DX(HW.pContext->Map(t_dst, d_sub, D3D_MAP_WRITE, 0, &map_dst));
#else
		CHK_DX(t_src0->Map(d_sub, D3D_MAP_READ, 0, &map_src0));
		CHK_DX(t_src1->Map(d_sub, D3D_MAP_READ, 0, &map_src1));
		CHK_DX(t_dst->Map(d_sub, D3D_MAP_WRITE, 0, &map_dst));
#endif
		u32 m_w = desc.Width >> i;
		u32 m_h = desc.Height >> i;
		if (m_w == 0) m_w = 1;
		if (m_h == 0) m_h = 1;
		pred(m_w, m_h, map_dst.RowPitch, map_src0.RowPitch, map_src1.RowPitch,
		     (u8*)map_dst.pData, (u8*)map_src0.pData, (u8*)map_src1.pData);
#ifdef USE_DX11
		HW.pContext->Unmap(t_src0, d_sub);
		HW.pContext->Unmap(t_src1, d_sub);
		HW.pContext->Unmap(t_dst, d_sub);
#else
		t_src0->Unmap(d_sub);
		t_src1->Unmap(d_sub);
		t_dst->Unmap(d_sub);
#endif
	}
}

IC u32 GetPowerOf2Plus1(u32 v)
{
	u32 cnt = 0;
	while (v)
	{
		v >>= 1;
		cnt++;
	};
	return cnt;
}

void TW_Save(ID3DTexture2D* t, LPCSTR name, LPCSTR prefix, LPCSTR postfix)
{
	string256 fn;
	strconcat(sizeof(fn), fn, name, "_", prefix, "-", postfix);
	for (int i = 0; i < int(xr_strlen(fn)); ++i)
		if ('\\' == fn[i]) fn[i] = '_';
	string256 fn2;
	strconcat(sizeof(fn2), fn2, "debug\\", fn, ".dds");
	Log("* debug texture save: ", fn2);
#ifdef USE_DX11
	R_CHK(D3DX11SaveTextureToFile(HW.pContext, t, D3DX11_IFF_DDS, fn2));
#else
	R_CHK(D3DX10SaveTextureToFile(t, D3DX10_IFF_DDS, fn2));
#endif
}

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize, bool bStaging)
{
	const bool bAllowStaging = !RImplementation.o.no_ram_textures;
	bStaging &= bAllowStaging;

	ID3DBaseTexture* pTexture2D = NULL;
	string_path fn;
	u32 img_size = 0;
	int img_loaded_lod = 0;
	u32 mip_cnt = u32(-1);

	string_path fname;
	xr_strcpy(fname, fRName);
	fix_texture_name(fname);
	IReader* S = NULL;
	if (FS.exist(fn, "$game_textures$", fname, ".dds")) goto _DDS;
	if (FS.exist(fn, "$level$", fname, ".dds")) goto _DDS;
	if (FS.exist(fn, "$game_saves$", fname, ".dds")) goto _DDS;
	if (strstr(fname, "_bump")) goto _BUMP_from_base;

#ifdef _EDITOR
	ELog.DlgMsg(mtError, "Can't find texture '%s'", fname);
	return 0;
#else
	Msg("! Can't find texture '%s'", fname);
	R_ASSERT(FS.exist(fn, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"));
	goto _DDS;
#endif

_DDS:
	{
		S = FS.r_open(fn);
#ifdef DEBUG
		Msg("* Loaded: %s[%d]b", fn, S->length());
#endif
		img_size = S->length();
		R_ASSERT(S);

		pTexture2D = NativeCreateTextureFromDDS(
			reinterpret_cast<const u8*>(S->pointer()),
			img_size,
			bStaging,
			fn,
			ret_msize,
			img_loaded_lod
		);
		if (pTexture2D)
		{
			FS.r_close(S);
			return pTexture2D;
		}

		g_fallback_dds_loaded.fetch_add(1, std::memory_order_relaxed);
		Msg("! [NativeDDS] Fallback to D3DX11 for non-standard texture: %s", fn);

#ifdef USE_DX11
		D3DX11_IMAGE_INFO IMG;
		HRESULT const result = D3DX11GetImageInfoFromMemory(S->pointer(), S->length(), 0, &IMG, 0);
#else
		D3DX10_IMAGE_INFO IMG;
		HRESULT const result = D3DX10GetImageInfoFromMemory(S->pointer(), S->length(), 0, &IMG, 0);
#endif
		if (FAILED(result))
		{
			Msg("! Can't get image info for texture '%s'", fn);
			FS.r_close(S);
			string_path temp;
			R_ASSERT(FS.exist(temp, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"));
			R_ASSERT(xr_strcmp(temp, fn));
			xr_strcpy(fn, temp);
			goto _DDS;
		}

		if (IMG.ResourceDimension == D3D_RESOURCE_DIMENSION_TEXTURE2D && (IMG.MiscFlags & D3D_RESOURCE_MISC_TEXTURECUBE))
			goto _DDS_CUBE;
		else
			goto _DDS_2D;

	_DDS_CUBE:
		{
#ifdef USE_DX11
			D3DX11_IMAGE_LOAD_INFO LoadInfo;
#else
			D3DX10_IMAGE_LOAD_INFO LoadInfo;
#endif
			if (bStaging)
			{
				LoadInfo.Usage = D3D_USAGE_STAGING;
				LoadInfo.BindFlags = 0;
				LoadInfo.CpuAccessFlags = D3D_CPU_ACCESS_WRITE;
			}
			else
			{
				LoadInfo.Usage = D3D_USAGE_DEFAULT;
				LoadInfo.BindFlags = D3D_BIND_SHADER_RESOURCE;
			}

			LoadInfo.pSrcInfo = &IMG;

#ifdef USE_DX11
			HRESULT const _hr = D3DX11CreateTextureFromMemory(HW.pDevice, S->pointer(), S->length(), &LoadInfo, 0, &pTexture2D, 0);
#else
			HRESULT const _hr = D3DX10CreateTextureFromMemory(HW.pDevice, S->pointer(), S->length(), &LoadInfo, 0, &pTexture2D, 0);
#endif
			FS.r_close(S);

			if (FAILED(_hr))
			{
				Msg("! Can't load texture '%s'", fn);
				string_path temp;
				R_ASSERT(FS.exist(temp, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"));
				R_ASSERT(xr_strcmp(temp, fn));
				xr_strcpy(fn, temp);
				goto _DDS;
			}

			mip_cnt = IMG.MipLevels;
			ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
			return pTexture2D;
		}

	_DDS_2D:
		{
			strlwr(fn);

#ifdef USE_DX11
			D3DX11_IMAGE_LOAD_INFO LoadInfo;
#else
			D3DX10_IMAGE_LOAD_INFO LoadInfo;
#endif
			LoadInfo.FirstMipLevel = img_loaded_lod;
			LoadInfo.Width = IMG.Width;
			LoadInfo.Height = IMG.Height;

			if (bStaging)
			{
				LoadInfo.Usage = D3D_USAGE_STAGING;
				LoadInfo.BindFlags = 0;
				LoadInfo.CpuAccessFlags = D3D_CPU_ACCESS_WRITE;
			}
			else
			{
				LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
				LoadInfo.BindFlags = D3D_BIND_SHADER_RESOURCE;
			}
			LoadInfo.pSrcInfo = &IMG;

#ifdef USE_DX11
			HRESULT const _hr = D3DX11CreateTextureFromMemory(HW.pDevice, S->pointer(), S->length(), &LoadInfo, 0, &pTexture2D, 0);
#else
			HRESULT const _hr = D3DX10CreateTextureFromMemory(HW.pDevice, S->pointer(), S->length(), &LoadInfo, 0, &pTexture2D, 0);
#endif
			FS.r_close(S);

			if (FAILED(_hr))
			{
				Msg("! Can't load texture '%s'", fn);
				string_path temp;
				R_ASSERT(FS.exist(temp, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"));
				xr_strlwr(temp);
				R_ASSERT(xr_strcmp(temp, fn));
				xr_strcpy(fn, temp);
				goto _DDS;
			}
			mip_cnt = IMG.MipLevels;
			ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
			return pTexture2D;
		}
	}

_BUMP_from_base:
	{
		Msg("! auto-generated bump map: %s", fname);
		if (strstr(fname, "_bump#"))
		{
			R_ASSERT2(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
			S = FS.r_open(fn);
			R_ASSERT2(S, fn);
			img_size = S->length();
			goto _DDS;
		}
		if (strstr(fname, "_bump"))
		{
			R_ASSERT2(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump", ".dds"), "ed_dummy_bump");
			S = FS.r_open(fn);
			R_ASSERT2(S, fn);
			img_size = S->length();
			goto _DDS;
		}
		return 0;
	}
}