/*

This file is provided under a dual BSD/GPLv2 license.  When using or
redistributing this file, you may do so under either license.

GPL LICENSE SUMMARY

Copyright(c) Oct. 2015 Intel Corporation.

This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

Contact Information:

Seung-Woo Kim, seung-woo.kim@intel.com
705 5th Ave S #500, Seattle, WA 98104

BSD LICENSE

Copyright(c) <date> Intel Corporation.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the
distribution.

* Neither the name of Intel Corporation nor the names of its
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "QSV_HevcEncoder_Internal.h"

#include <VersionHelpers.h>
#include <obs-module.h>

#include "mfxastructures.h"
#include "mfxvideo++.h"

#define do_log(level, format, ...) \
	blog(level, "[qsv encoder: '%s'] " format, "msdk_impl", ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

mfxHDL QSV_HevcEncoder_Internal::g_DX_Handle = NULL;
mfxU16 QSV_HevcEncoder_Internal::g_numEncodersOpen = 0;

QSV_HevcEncoder_Internal::QSV_HevcEncoder_Internal(mfxIMPL &impl,
						   mfxVersion &version)
	: m_pmfxSurfaces(NULL),
	  m_pmfxENC(NULL),
	  m_nTaskPool(0),
	  m_pTaskPool(NULL),
	  m_nTaskIdx(0),
	  m_nFirstSyncTask(0),
	  m_outBitstream()
{
	mfxIMPL tempImpl;
	mfxStatus sts;

	m_bIsWindows8OrGreater = IsWindows8OrGreater();
	m_bUseD3D11 = false;
	m_bD3D9HACK = true;

	if (m_bIsWindows8OrGreater) {
		tempImpl = impl | MFX_IMPL_VIA_D3D11;
		sts = m_session.Init(tempImpl, &version);
		if (sts == MFX_ERR_NONE) {
			m_session.QueryVersion(&version);
			m_session.Close();

			// Use D3D11 surface
			m_bUseD3D11 =
				((version.Major > 1) ||
				 (version.Major == 1 && version.Minor >= 8));
			m_bD3D9HACK = !m_bUseD3D11;

			if (m_bUseD3D11)
				blog(LOG_INFO, "\timpl:           D3D11\n"
					       "\tsurf:           D3D11");
			else
				blog(LOG_INFO, "\timpl:           D3D11\n"
					       "\tsurf:           SysMem");

			m_impl = tempImpl;
			m_ver = version;
			return;
		}
	} else if (m_bD3D9HACK) {
		tempImpl = impl | MFX_IMPL_VIA_D3D9;
		sts = m_session.Init(tempImpl, &version);
		if (sts == MFX_ERR_NONE) {
			m_session.QueryVersion(&version);
			m_session.Close();

			blog(LOG_INFO, "\timpl:           D3D09\n"
				       "\tsurf:           Hack");

			m_impl = tempImpl;
			m_ver = version;
			return;
		}
	}

	// Either windows 7 or D3D11 failed at this point.
	tempImpl = impl | MFX_IMPL_VIA_D3D9;
	sts = m_session.Init(tempImpl, &version);
	if (sts == MFX_ERR_NONE) {
		m_session.QueryVersion(&version);
		m_session.Close();

		blog(LOG_INFO, "\timpl:           D3D09\n"
			       "\tsurf:           SysMem");

		m_impl = tempImpl;
		m_ver = version;
	}
}

QSV_HevcEncoder_Internal::~QSV_HevcEncoder_Internal()
{
	if (m_pmfxENC)
		ClearData();
}

mfxStatus QSV_HevcEncoder_Internal::Open(qsv_param_t *pParams)
{
	mfxStatus sts = MFX_ERR_NONE;

	if (m_bUseD3D11)
		// Use D3D11 surface
		sts = Initialize(m_impl, m_ver, &m_session, &m_mfxAllocator,
				 &g_DX_Handle, false, false);
	else if (m_bD3D9HACK)
		// Use hack
		sts = Initialize(m_impl, m_ver, &m_session, &m_mfxAllocator,
				 &g_DX_Handle, false, true);
	else
		sts = Initialize(m_impl, m_ver, &m_session, NULL);

	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	m_pmfxENC = new MFXVideoENCODE(m_session);

	sts = InitParams(pParams);
	MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_pmfxENC->Query(&m_mfxEncParams, &m_mfxEncParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = AllocateSurfaces();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_pmfxENC->Init(&m_mfxEncParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = GetVideoParam();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = InitBitstream();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (sts >= MFX_ERR_NONE) {
		g_numEncodersOpen++;
	}
	return sts;
}
void QSV_HevcEncoder_Internal::GetVpsSpsPps(mfxU8 **pVPSBuf, mfxU8 **pSPSBuf,
					    mfxU8 **pPPSBuf, mfxU16 *pnVPSBuf,
					    mfxU16 *pnSPSBuf, mfxU16 *pnPPSBuf)
{
	*pVPSBuf = m_VPSBuffer;
	*pnVPSBuf = m_nVPSBufferSize;

	*pSPSBuf = m_SPSBuffer;
	*pnSPSBuf = m_nSPSBufferSize;

	*pPPSBuf = m_PPSBuffer;
	*pnPPSBuf = m_nPPSBufferSize;
}
mfxStatus QSV_HevcEncoder_Internal::InitParams(qsv_param_t *pParams)
{
	m_mfxEncParams = mfxVideoParam{};
	mfxInfoMFX &mfx = m_mfxEncParams.mfx;
	mfx.CodecId = MFX_CODEC_HEVC;
	mfx.GopOptFlag = MFX_GOP_STRICT /*MFX_GOP_CLOSED*/;
	mfx.GopRefDist = pParams->nbFrames + 1;
	mfx.NumSlice = 0;
	mfx.TargetUsage = pParams->nTargetUsage;
	mfx.CodecProfile = pParams->nCodecProfile;
	if (mfx.CodecProfile == MFX_PROFILE_HEVC_MAIN10) {
		mfx.FrameInfo.FourCC = MFX_FOURCC_P010;
		mfx.FrameInfo.BitDepthLuma = 10;
		mfx.FrameInfo.BitDepthChroma = 10;
		mfx.FrameInfo.Shift = 1;
	} else {
		mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
	}

	mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
	mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	mfx.IdrInterval = 1;
	mfx.FrameInfo.FrameRateExtN = pParams->nFpsNum;
	mfx.FrameInfo.FrameRateExtD = pParams->nFpsDen;

	mfx.FrameInfo.CropX = 0;
	mfx.FrameInfo.CropY = 0;
	mfx.FrameInfo.CropW = pParams->nWidth;
	mfx.FrameInfo.CropH = pParams->nHeight;
	// Width must be a multiple of 16
	// Height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
	mfx.FrameInfo.Width = (pParams->nWidth);
	mfx.FrameInfo.Height = MSDK_ALIGN32(pParams->nHeight);
	if (MFX_PICSTRUCT_PROGRESSIVE == mfx.FrameInfo.PicStruct) {
		MSDK_ALIGN16(pParams->nHeight);
	}

	mfx.EncodedOrder = 0;
	mfx.LowPower = MFX_CODINGOPTION_OFF;

	enum qsv_cpu_platform qsv_platform = qsv_get_cpu_platform();
	if ((qsv_platform >= QSV_CPU_PLATFORM_ICL) &&
	    (pParams->nbFrames == 0) &&
	    (m_ver.Major == 1 && m_ver.Minor >= 31)) {
		mfx.LowPower = MFX_CODINGOPTION_ON;
		if (pParams->nRateControl == MFX_RATECONTROL_LA_ICQ ||
		    pParams->nRateControl == MFX_RATECONTROL_LA_HRD ||
		    pParams->nRateControl == MFX_RATECONTROL_LA)
			pParams->nRateControl = MFX_RATECONTROL_VBR;
	}

	mfx.RateControlMethod = pParams->nRateControl;

	switch (pParams->nRateControl) {
	case MFX_RATECONTROL_CBR:
		mfx.TargetKbps = pParams->nTargetBitRate;
		break;
	case MFX_RATECONTROL_VBR:
	case MFX_RATECONTROL_VCM:
		mfx.TargetKbps = pParams->nTargetBitRate;
		mfx.MaxKbps = pParams->nMaxBitRate;
		break;
	case MFX_RATECONTROL_CQP:
		mfx.QPI = pParams->nQPI;
		mfx.QPB = pParams->nQPB;
		mfx.QPP = pParams->nQPP;
		break;
	case MFX_RATECONTROL_AVBR:
		mfx.TargetKbps = pParams->nTargetBitRate;
		mfx.Accuracy = pParams->nAccuracy;
		mfx.Convergence = pParams->nConvergence;
		break;
	case MFX_RATECONTROL_ICQ:
		mfx.ICQQuality = pParams->nICQQuality;
		break;
	case MFX_RATECONTROL_LA:
		mfx.TargetKbps = pParams->nTargetBitRate;
		break;
	case MFX_RATECONTROL_LA_ICQ:
		mfx.ICQQuality = pParams->nICQQuality;
		break;
	case MFX_RATECONTROL_LA_HRD:
		mfx.TargetKbps = pParams->nTargetBitRate;
		mfx.MaxKbps = pParams->nTargetBitRate;
		break;
	default:
		break;
	}

	m_mfxEncParams.AsyncDepth = pParams->nAsyncDepth;
	int key_int_sec = min(2, pParams->nKeyIntSec);
	mfx.GopPicSize = (mfxU16)(key_int_sec * (float)pParams->nFpsNum /
				  (float)pParams->nFpsDen);

	static mfxExtBuffer *extendedBuffers[6];
	int iBuffers = 0;

	if (m_ver.Major == 1 && m_ver.Minor >= 8) {
		m_co2 = mfxExtCodingOption2{};
		m_co2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
		m_co2.Header.BufferSz = sizeof(mfxExtCodingOption2);
		m_co2.RepeatPPS = MFX_CODINGOPTION_ON;

		if (pParams->nRateControl == MFX_RATECONTROL_LA_ICQ ||
		    pParams->nRateControl == MFX_RATECONTROL_LA)
			m_co2.LookAheadDepth = pParams->nLADEPTH;
		if (pParams->bMBBRC)
			m_co2.MBBRC = MFX_CODINGOPTION_ON;
		if (pParams->nbFrames > 1)
			m_co2.BRefType = MFX_B_REF_PYRAMID;
		if (mfx.LowPower == MFX_CODINGOPTION_ON) {
			// m_co2.RepeatPPS = MFX_CODINGOPTION_OFF;
			if (pParams->nRateControl == MFX_RATECONTROL_CBR ||
			    pParams->nRateControl == MFX_RATECONTROL_VBR) {
				m_co2.LookAheadDepth = pParams->nLADEPTH;
			}
		}
		extendedBuffers[iBuffers++] = (mfxExtBuffer *)&m_co2;
	}

	if (mfx.LowPower == MFX_CODINGOPTION_ON ||
	    (pParams->bCQM && m_ver.Major == 1 && m_ver.Minor >= 16)) {
		m_co3 = mfxExtCodingOption3{};
		m_co3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
		m_co3.Header.BufferSz = sizeof(mfxExtCodingOption3);
		m_co3.ScenarioInfo = MFX_SCENARIO_GAME_STREAMING;
		m_co3.GPB = MFX_CODINGOPTION_OFF;
		extendedBuffers[iBuffers++] = (mfxExtBuffer *)&m_co3;
	}

	if ((!((mfx.FrameInfo.CropW & 15) ^ 8) ||
	     !((mfx.FrameInfo.CropH & 15) ^ 8))) {
		m_ExtHEVCParam = mfxExtHEVCParam{};
		m_ExtHEVCParam.Header.BufferId = MFX_EXTBUFF_HEVC_PARAM;
		m_ExtHEVCParam.Header.BufferSz = sizeof(mfxExtHEVCParam);
		m_ExtHEVCParam.PicWidthInLumaSamples = mfx.FrameInfo.CropW;
		m_ExtHEVCParam.PicHeightInLumaSamples = mfx.FrameInfo.CropH;
		extendedBuffers[iBuffers++] = (mfxExtBuffer *)&m_ExtHEVCParam;
	}

	const mfxU16 kTransferCharacteristicsSmpteST2084 = 16;
	const mfxU16 kTransferCharacteristicsAribSTD_B67 = 18;
	const mfxU16 kColourPrimariesBT2020 = 9;
	const mfxU16 kMatrixCoefficientsBT2020ncl = 9;

	if (mfx.CodecProfile == MFX_PROFILE_HEVC_MAIN10) {
		m_ExtVideoSignalInfo = mfxExtVideoSignalInfo{};
		m_ExtVideoSignalInfo.Header.BufferId =
			MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
		m_ExtVideoSignalInfo.Header.BufferSz =
			sizeof(mfxExtVideoSignalInfo);

		m_ExtVideoSignalInfo.ColourDescriptionPresent = true;
		m_ExtVideoSignalInfo.VideoFormat = 5;
		m_ExtVideoSignalInfo.VideoFullRange = false;
		if (pParams->bHLG) { // HLG
			m_ExtVideoSignalInfo.ColourPrimaries =
				kColourPrimariesBT2020;
			m_ExtVideoSignalInfo.TransferCharacteristics =
				kTransferCharacteristicsAribSTD_B67;
			m_ExtVideoSignalInfo.MatrixCoefficients =
				kMatrixCoefficientsBT2020ncl;
		} else { // PQ
			m_ExtVideoSignalInfo.ColourPrimaries =
				kColourPrimariesBT2020;
			m_ExtVideoSignalInfo.TransferCharacteristics =
				kTransferCharacteristicsSmpteST2084;
			m_ExtVideoSignalInfo.MatrixCoefficients =
				kMatrixCoefficientsBT2020ncl;
		}

		extendedBuffers[iBuffers++] =
			(mfxExtBuffer *)&m_ExtVideoSignalInfo;
	}

	if (iBuffers > 0) {
		m_mfxEncParams.ExtParam = extendedBuffers;
		m_mfxEncParams.NumExtParam = (mfxU16)iBuffers;
	}

	m_mfxEncParams.IOPattern =
		(m_bUseD3D11 || m_bD3D9HACK)
			? mfxU16(MFX_IOPATTERN_IN_VIDEO_MEMORY)
			: mfxU16(MFX_IOPATTERN_IN_SYSTEM_MEMORY);

	mfxStatus sts = m_pmfxENC->Query(&m_mfxEncParams, &m_mfxEncParams);
	if (sts == MFX_ERR_UNSUPPORTED || sts == MFX_ERR_UNDEFINED_BEHAVIOR) {
		if (mfx.LowPower == MFX_CODINGOPTION_ON) {
			mfx.LowPower = MFX_CODINGOPTION_OFF;
			m_co2.LookAheadDepth = 0;
			sts = m_pmfxENC->Query(&m_mfxEncParams,
					       &m_mfxEncParams);
		}
	}

	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::AllocateSurfaces()
{
	// Query number of required surfaces for encoder
	mfxFrameAllocRequest EncRequest;
	memset(&EncRequest, 0, sizeof(EncRequest));
	mfxStatus sts = m_pmfxENC->QueryIOSurf(&m_mfxEncParams, &EncRequest);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	EncRequest.Type |= WILL_WRITE;

	// SNB hack. On some SNB, it seems to require more surfaces
	EncRequest.NumFrameSuggested += m_mfxEncParams.AsyncDepth;

	// Allocate required surfaces
	if (m_bUseD3D11 || m_bD3D9HACK) {
		sts = m_mfxAllocator.Alloc(m_mfxAllocator.pthis, &EncRequest,
					   &m_mfxResponse);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		m_nSurfNum = m_mfxResponse.NumFrameActual;

		m_pmfxSurfaces = new mfxFrameSurface1 *[m_nSurfNum];
		MSDK_CHECK_POINTER(m_pmfxSurfaces, MFX_ERR_MEMORY_ALLOC);

		for (int i = 0; i < m_nSurfNum; i++) {
			m_pmfxSurfaces[i] = new mfxFrameSurface1;
			memset(m_pmfxSurfaces[i], 0, sizeof(mfxFrameSurface1));
			memcpy(&(m_pmfxSurfaces[i]->Info),
			       &(m_mfxEncParams.mfx.FrameInfo),
			       sizeof(mfxFrameInfo));
			m_pmfxSurfaces[i]->Data.MemId = m_mfxResponse.mids[i];
		}
	} else {
		mfxU16 width = (mfxU16)MSDK_ALIGN32(EncRequest.Info.Width);
		mfxU16 height = (mfxU16)MSDK_ALIGN32(EncRequest.Info.Height);
		mfxU8 bitsPerPixel = 12;
		mfxU32 surfaceSize = width * height * bitsPerPixel / 8;
		m_nSurfNum = EncRequest.NumFrameSuggested;

		m_pmfxSurfaces = new mfxFrameSurface1 *[m_nSurfNum];
		for (int i = 0; i < m_nSurfNum; i++) {
			m_pmfxSurfaces[i] = new mfxFrameSurface1;
			memset(m_pmfxSurfaces[i], 0, sizeof(mfxFrameSurface1));
			memcpy(&(m_pmfxSurfaces[i]->Info),
			       &(m_mfxEncParams.mfx.FrameInfo),
			       sizeof(mfxFrameInfo));

			mfxU8 *pSurface = (mfxU8 *)new mfxU8[surfaceSize];
			m_pmfxSurfaces[i]->Data.Y = pSurface;
			m_pmfxSurfaces[i]->Data.U = pSurface + width * height;
			m_pmfxSurfaces[i]->Data.V =
				pSurface + width * height + 1;
			m_pmfxSurfaces[i]->Data.Pitch = width;
		}
	}

	blog(LOG_INFO, "\tm_nSurfNum:     %d", m_nSurfNum);

	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::GetVideoParam()
{
	memset(&m_parameter, 0, sizeof(m_parameter));

	mfxExtCodingOptionVPS option_vps{};
	option_vps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_VPS;
	option_vps.Header.BufferSz = sizeof(mfxExtCodingOptionVPS);

	mfxExtCodingOptionSPSPPS option_sps_pps{};
	option_sps_pps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
	option_sps_pps.Header.BufferSz = sizeof(mfxExtCodingOptionSPSPPS);

	static mfxExtBuffer *extendedBuffers[2];
	extendedBuffers[0] = (mfxExtBuffer *)&option_vps;
	extendedBuffers[1] = (mfxExtBuffer *)&option_sps_pps;
	m_parameter.ExtParam = extendedBuffers;
	m_parameter.NumExtParam = 2;

	option_vps.VPSBuffer = m_VPSBuffer;
	option_vps.VPSBufSize = kVPSBufferCapacity;

	option_sps_pps.SPSBuffer = m_SPSBuffer;
	option_sps_pps.SPSBufSize = kSPSBufferCapacity;

	option_sps_pps.PPSBuffer = m_PPSBuffer;
	option_sps_pps.PPSBufSize = kPPSBufferCapacity;

	mfxStatus sts = m_pmfxENC->GetVideoParam(&m_parameter);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	m_nVPSBufferSize = option_vps.VPSBufSize;
	m_nSPSBufferSize = option_sps_pps.SPSBufSize;
	m_nPPSBufferSize = option_sps_pps.PPSBufSize;

	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::InitBitstream()
{
	m_nTaskPool = m_parameter.AsyncDepth;
	m_nFirstSyncTask = 0;

	m_pTaskPool = new Task[m_nTaskPool];
	memset(m_pTaskPool, 0, sizeof(Task) * m_nTaskPool);

	for (int i = 0; i < m_nTaskPool; i++) {
		m_pTaskPool[i].mfxBS.MaxLength =
			m_parameter.mfx.BufferSizeInKB * 1000;
		m_pTaskPool[i].mfxBS.Data =
			new mfxU8[m_pTaskPool[i].mfxBS.MaxLength];
		m_pTaskPool[i].mfxBS.DataOffset = 0;
		m_pTaskPool[i].mfxBS.DataLength = 0;

		MSDK_CHECK_POINTER(m_pTaskPool[i].mfxBS.Data,
				   MFX_ERR_MEMORY_ALLOC);
	}

	memset(&m_outBitstream, 0, sizeof(mfxBitstream));
	m_outBitstream.MaxLength = m_parameter.mfx.BufferSizeInKB * 1000;
	m_outBitstream.Data = new mfxU8[m_outBitstream.MaxLength];
	m_outBitstream.DataOffset = 0;
	m_outBitstream.DataLength = 0;

	blog(LOG_INFO, "\tm_nTaskPool:    %d", m_nTaskPool);

	return MFX_ERR_NONE;
}

static void CopyPlane(mfxU8 *dest, mfxU32 dst_pitch, const uint8_t *src,
		      uint32_t src_pitch, mfxU32 line_size, mfxU32 lines_count)
{

	for (mfxU16 i = 0; i < lines_count; i++)
		memcpy(dest + i * dst_pitch, src + i * src_pitch, line_size);
}

mfxStatus QSV_HevcEncoder_Internal::LoadNV12(mfxFrameSurface1 *pSurface,
					     uint8_t *pDataY, uint8_t *pDataUV,
					     uint32_t strideY,
					     uint32_t strideUV)
{
	mfxU16 w, h;
	mfxFrameInfo *info = &pSurface->Info;
	mfxFrameData *dst = &pSurface->Data;

	if (info->CropH > 0 && info->CropW > 0) {
		w = info->CropW;
		h = info->CropH;
	} else {
		w = info->Width;
		h = info->Height;
	}
	mfxU32 line_size = w;
	mfxU32 dst_pitch = dst->Pitch;
	mfxU8 *dstY = dst->Y + info->CropX + info->CropY * dst_pitch;
	mfxU8 *dstUV = dst->UV + info->CropX + (info->CropY / 2) * dst_pitch;

	// load Y plane
	CopyPlane(dstY, dst_pitch, pDataY, strideY, line_size, h);
	// load UV plane
	CopyPlane(dstUV, dst_pitch, pDataUV, strideUV, line_size, h / 2);

	return MFX_ERR_NONE;
}

mfxStatus QSV_HevcEncoder_Internal::LoadP010(mfxFrameSurface1 *pSurface,
					     uint8_t *pDataY, uint8_t *pDataUV,
					     uint32_t strideY,
					     uint32_t strideUV)
{
	mfxU16 w, h;
	mfxFrameInfo *info = &pSurface->Info;
	mfxFrameData *dst = &pSurface->Data;

	if (info->CropH > 0 && info->CropW > 0) {
		w = info->CropW;
		h = info->CropH;
	} else {
		w = info->Width;
		h = info->Height;
	}
	mfxU32 line_size = w * 2;
	mfxU32 dst_pitch = dst->Pitch;
	mfxU8 *dstY = dst->Y + info->CropX + info->CropY * dst_pitch;
	mfxU8 *dstUV = dst->UV + info->CropX + (info->CropY / 2) * dst_pitch;

	// load Y plane
	CopyPlane(dstY, dst_pitch, pDataY, strideY, line_size, h);
	// load UV plane
	CopyPlane(dstUV, dst_pitch, pDataUV, strideUV, line_size, h / 2);

	return MFX_ERR_NONE;
}

int QSV_HevcEncoder_Internal::GetFreeTaskIndex(Task *pTaskPool,
					       mfxU16 nPoolSize)
{
	if (pTaskPool)
		for (int i = 0; i < nPoolSize; i++)
			if (!pTaskPool[i].syncp)
				return i;
	return MFX_ERR_NOT_FOUND;
}

mfxStatus QSV_HevcEncoder_Internal::Encode(uint64_t ts, uint8_t *pDataY,
					   uint8_t *pDataUV, uint32_t strideY,
					   uint32_t strideUV,
					   mfxBitstream **pBS)
{
	mfxStatus sts = MFX_ERR_NONE;
	*pBS = NULL;
	int nTaskIdx = GetFreeTaskIndex(m_pTaskPool, m_nTaskPool);

#if 0
	info("MSDK Encode:\n"
		"\tTaskIndex: %d",
		nTaskIdx);
#endif

	int nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);
#if 0
	info("MSDK Encode:\n"
		"\tnSurfIdx: %d",
		nSurfIdx);
#endif

	while (MFX_ERR_NOT_FOUND == nTaskIdx || MFX_ERR_NOT_FOUND == nSurfIdx) {
		// No more free tasks or surfaces, need to sync
		sts = m_session.SyncOperation(
			m_pTaskPool[m_nFirstSyncTask].syncp, 60000);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		mfxU8 *pTemp = m_outBitstream.Data;
		memcpy(&m_outBitstream, &m_pTaskPool[m_nFirstSyncTask].mfxBS,
		       sizeof(mfxBitstream));

		m_pTaskPool[m_nFirstSyncTask].mfxBS.Data = pTemp;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataLength = 0;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataOffset = 0;
		m_pTaskPool[m_nFirstSyncTask].syncp = NULL;
		nTaskIdx = m_nFirstSyncTask;
		m_nFirstSyncTask = (m_nFirstSyncTask + 1) % m_nTaskPool;
		*pBS = &m_outBitstream;

#if 0
		info("MSDK Encode:\n"
			"\tnew FirstSyncTask: %d\n"
			"\tTaskIndex:         %d",
			m_nFirstSyncTask,
			nTaskIdx);
#endif

		nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);
#if 0
		info("MSDK Encode:\n"
			"\tnSurfIdx: %d",
			nSurfIdx);
#endif
	}

	mfxFrameSurface1 *pSurface = m_pmfxSurfaces[nSurfIdx];
	if (m_bUseD3D11 || m_bD3D9HACK) {
		sts = m_mfxAllocator.Lock(m_mfxAllocator.pthis,
					  pSurface->Data.MemId,
					  &(pSurface->Data));
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	if (pSurface->Info.FourCC == MFX_FOURCC_P010)
		sts = LoadP010(pSurface, pDataY, pDataUV, strideY, strideUV);
	else
		sts = LoadNV12(pSurface, pDataY, pDataUV, strideY, strideUV);

	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	pSurface->Data.TimeStamp = ts;

	if (m_bUseD3D11 || m_bD3D9HACK) {
		sts = m_mfxAllocator.Unlock(m_mfxAllocator.pthis,
					    pSurface->Data.MemId,
					    &(pSurface->Data));
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	for (;;) {
		// Encode a frame asynchronously (returns immediately)
		sts = m_pmfxENC->EncodeFrameAsync(NULL, pSurface,
						  &m_pTaskPool[nTaskIdx].mfxBS,
						  &m_pTaskPool[nTaskIdx].syncp);

		if (MFX_ERR_NONE < sts && !m_pTaskPool[nTaskIdx].syncp) {
			// Repeat the call if warning and no output
			if (MFX_WRN_DEVICE_BUSY == sts)
				MSDK_SLEEP(
					1); // Wait if device is busy, then repeat the same call
		} else if (MFX_ERR_NONE < sts && m_pTaskPool[nTaskIdx].syncp) {
			sts = MFX_ERR_NONE; // Ignore warnings if output is available
			break;
		} else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
			// Allocate more bitstream buffer memory here if needed...
			break;
		} else
			break;
	}

	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::Encode_tex(uint64_t ts, uint32_t tex_handle,
					       uint64_t lock_key,
					       uint64_t *next_key,
					       mfxBitstream **pBS)
{
	mfxStatus sts = MFX_ERR_NONE;
	*pBS = NULL;
	int nTaskIdx = GetFreeTaskIndex(m_pTaskPool, m_nTaskPool);
	int nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);

	while (MFX_ERR_NOT_FOUND == nTaskIdx || MFX_ERR_NOT_FOUND == nSurfIdx) {
		// No more free tasks or surfaces, need to sync
		sts = m_session.SyncOperation(
			m_pTaskPool[m_nFirstSyncTask].syncp, 60000);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		mfxU8 *pTemp = m_outBitstream.Data;
		memcpy(&m_outBitstream, &m_pTaskPool[m_nFirstSyncTask].mfxBS,
		       sizeof(mfxBitstream));

		m_pTaskPool[m_nFirstSyncTask].mfxBS.Data = pTemp;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataLength = 0;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataOffset = 0;
		m_pTaskPool[m_nFirstSyncTask].syncp = NULL;
		nTaskIdx = m_nFirstSyncTask;
		m_nFirstSyncTask = (m_nFirstSyncTask + 1) % m_nTaskPool;
		*pBS = &m_outBitstream;

		nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);
	}

	mfxFrameSurface1 *pSurface = m_pmfxSurfaces[nSurfIdx];
	//copy to default surface directly
	pSurface->Data.TimeStamp = ts;
	if (m_bUseD3D11 || m_bD3D9HACK) {
		sts = simple_copytex(m_mfxAllocator.pthis, pSurface->Data.MemId,
				     tex_handle, lock_key, next_key);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	for (;;) {
		// Encode a frame asynchronously (returns immediately)
		sts = m_pmfxENC->EncodeFrameAsync(NULL, pSurface,
						  &m_pTaskPool[nTaskIdx].mfxBS,
						  &m_pTaskPool[nTaskIdx].syncp);

		if (MFX_ERR_NONE < sts && !m_pTaskPool[nTaskIdx].syncp) {
			// Repeat the call if warning and no output
			if (MFX_WRN_DEVICE_BUSY == sts)
				MSDK_SLEEP(
					1); // Wait if device is busy, then repeat the same call
		} else if (MFX_ERR_NONE < sts && m_pTaskPool[nTaskIdx].syncp) {
			sts = MFX_ERR_NONE; // Ignore warnings if output is available
			break;
		} else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
			// Allocate more bitstream buffer memory here if needed...
			break;
		} else
			break;
	}

	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::Drain()
{
	mfxStatus sts = MFX_ERR_NONE;

	while (m_pTaskPool && m_pTaskPool[m_nFirstSyncTask].syncp) {
		sts = m_session.SyncOperation(
			m_pTaskPool[m_nFirstSyncTask].syncp, 60000);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		m_pTaskPool[m_nFirstSyncTask].syncp = NULL;
		m_nFirstSyncTask = (m_nFirstSyncTask + 1) % m_nTaskPool;
	}

	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::ClearData()
{
	mfxStatus sts = MFX_ERR_NONE;
	sts = Drain();

	if (m_pmfxENC) {
		sts = m_pmfxENC->Close();
		delete m_pmfxENC;
		m_pmfxENC = NULL;
	}

	if (m_bUseD3D11 || m_bD3D9HACK)
		m_mfxAllocator.Free(m_mfxAllocator.pthis, &m_mfxResponse);

	if (m_pmfxSurfaces) {
		for (int i = 0; i < m_nSurfNum; i++) {
			if (!m_bUseD3D11 && !m_bD3D9HACK)
				delete m_pmfxSurfaces[i]->Data.Y;

			delete m_pmfxSurfaces[i];
		}
		MSDK_SAFE_DELETE_ARRAY(m_pmfxSurfaces);
	}

	if (m_pTaskPool) {
		for (int i = 0; i < m_nTaskPool; i++)
			delete m_pTaskPool[i].mfxBS.Data;
		MSDK_SAFE_DELETE_ARRAY(m_pTaskPool);
	}

	if (m_outBitstream.Data) {
		delete m_outBitstream.Data;
		m_outBitstream.Data = NULL;
	}

	if (sts >= MFX_ERR_NONE) {
		g_numEncodersOpen--;
	}

	if ((m_bUseD3D11 || m_bD3D9HACK) && (g_numEncodersOpen <= 0)) {
		Release();
		g_DX_Handle = NULL;
	}
	m_session.Close();
	return sts;
}

mfxStatus QSV_HevcEncoder_Internal::Reset(qsv_param_t *pParams)
{
	mfxStatus sts = ClearData();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = Open(pParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	return sts;
}
