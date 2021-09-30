/*############################################################################
  # Copyright (C) Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include <algorithm>
#include "mfx_samples_config.h"
#include "sample_defs.h"

#if defined(_WIN32) || defined(_WIN64)
    #include <tchar.h>
    #include <windows.h>
#endif

#include <algorithm>
#include <ctime>
#include <thread>
#include "pipeline_decode.h"
#include "sysmem_allocator.h"

#if defined(_WIN32) || defined(_WIN64)
    #include "d3d11_allocator.h"
    #include "d3d11_device.h"
    #include "d3d_allocator.h"
    #include "d3d_device.h"
#endif

#if defined LIBVA_SUPPORT
    #include "vaapi_allocator.h"
    #include "vaapi_device.h"
    #include "vaapi_utils.h"
#endif

#if defined(LIBVA_WAYLAND_SUPPORT)
    #include "class_wayland.h"
#endif

#include "version.h"

#define __SYNC_WA // avoid sync issue on Media SDK side

#ifndef MFX_VERSION
    #error MFX_VERSION not defined
#endif

CDecodingPipeline::CDecodingPipeline()
        : m_FileWriter(),
          m_FileReader(),
          m_mfxBS(8 * 1024 * 1024),
          totalBytesProcessed(0),
          m_mfxSession(),
          m_impl(),
          m_pmfxDEC(NULL),
          m_pmfxVPP(NULL)
#if (MFX_VERSION >= 2000)
          ,
          m_pmfxMemory(NULL),
          m_pmfxDecVPP(NULL),
          m_mfxLoader(),
          m_bAPI2XInternalMem(false),
          m_bAPI2XDecVPP(false),
          m_bAPI2XPerf(false),
          m_api2xPerfLoopTime(0),
          m_numVPPCh(0),
          m_pmfxVPPChParams(NULL),
          m_pDecVPPOutSurfaces(NULL)
#endif
          ,
          m_mfxVideoParams(),
          m_mfxVppVideoParams()
#if (MFX_VERSION < 2000)
          ,
          m_pUserModule(),
          m_pPlugin()
#endif
          ,
          m_pGeneralAllocator(NULL),
          m_pmfxAllocatorParams(NULL),
          m_memType(SYSTEM_MEMORY),
          m_bExternalAlloc(false),
          m_bDecOutSysmem(false),
          m_mfxResponse{ 0 },
          m_mfxVppResponse{ 0 },
          m_pCurrentFreeSurface(NULL),
          m_pCurrentFreeVppSurface(NULL),
          m_pCurrentFreeOutputSurface(NULL),
          m_pCurrentOutputSurface(NULL),
          m_pDeliverOutputSemaphore(NULL),
          m_pDeliveredEvent(NULL),
          m_error(MFX_ERR_NONE),
          m_bStopDeliverLoop(false),
          m_eWorkMode(MODE_PERFORMANCE),
          m_bIsMVC(false),
          m_bIsVideoWall(false),
          m_bIsCompleteFrame(false),
          m_fourcc(0),
          m_bPrintLatency(false),
          m_bOutI420(false),
          m_vppOutWidth(0),
          m_vppOutHeight(0),
          m_nTimeout(0),
          m_nMaxFps(0),
          m_nFrames(0),
          m_diMode(0),
          m_bVppIsUsed(false),
          m_bVppFullColorRange(false),
          m_bSoftRobustFlag(false),
          m_vLatency(),
          m_startTick(0),
          m_delayTicks(0),
          m_VppVideoSignalInfo{ 0 },
          m_VppSurfaceExtParams()
#if defined(LIBVA_SUPPORT)
          ,
          m_strDevicePath()
#endif
          ,
          m_hwdev(NULL)
#if D3D_SURFACES_SUPPORT
          ,
          m_d3dRender()
#endif
          ,
          m_bRenderWin(false),
          m_nRenderWinX(0),
          m_nRenderWinY(0),
          m_nRenderWinW(0),
          m_nRenderWinH(0),
          m_export_mode(0),
          m_monitorType(0)
#if defined(LIBVA_SUPPORT)
          ,
          m_libvaBackend(0),
          m_bPerfMode(false)
#endif // defined(MFX_LIBVA_SUPPORT)
          ,
          m_bResetFileWriter(false),
          m_bResetFileReader(false) {
    MSDK_ZERO_MEMORY(m_VppVideoSignalInfo);
    m_VppVideoSignalInfo.Header.BufferId = MFX_EXTBUFF_VPP_VIDEO_SIGNAL_INFO;
    m_VppVideoSignalInfo.Header.BufferSz = sizeof(m_VppVideoSignalInfo);
    m_vLatency.reserve(
        1000); // reserve some space to reduce dynamic reallocation impact on pipeline execution
}

CDecodingPipeline::~CDecodingPipeline() {
    Close();
}

#if (defined(_WIN64) || defined(_WIN32)) && (MFX_VERSION >= 1031)
mfxU32 CDecodingPipeline::GetPreferredAdapterNum(const mfxAdaptersInfo& adapters,
                                                 const sInputParams& params) {
    if (adapters.NumActual == 0 || !adapters.Adapters)
        return 0;

    if (params.bPrefferdGfx) {
        // Find dGfx adapter in list and return it's index

        auto idx = std::find_if(adapters.Adapters,
                                adapters.Adapters + adapters.NumActual,
                                [](const mfxAdapterInfo info) {
                                    return info.Platform.MediaAdapterType ==
                                           mfxMediaAdapterType::MFX_MEDIA_DISCRETE;
                                });

        // No dGfx in list
        if (idx == adapters.Adapters + adapters.NumActual) {
            msdk_printf(
                MSDK_STRING("Warning: No dGfx detected on machine. Will pick another adapter\n"));
            return 0;
        }

        return static_cast<mfxU32>(std::distance(adapters.Adapters, idx));
    }

    if (params.bPrefferiGfx) {
        // Find iGfx adapter in list and return it's index

        auto idx = std::find_if(adapters.Adapters,
                                adapters.Adapters + adapters.NumActual,
                                [](const mfxAdapterInfo info) {
                                    return info.Platform.MediaAdapterType ==
                                           mfxMediaAdapterType::MFX_MEDIA_INTEGRATED;
                                });

        // No iGfx in list
        if (idx == adapters.Adapters + adapters.NumActual) {
            msdk_printf(
                MSDK_STRING("Warning: No iGfx detected on machine. Will pick another adapter\n"));
            return 0;
        }

        return static_cast<mfxU32>(std::distance(adapters.Adapters, idx));
    }

    // Other ways return 0, i.e. best suitable detected by dispatcher
    return 0;
}
#endif

mfxStatus CDecodingPipeline::GetImpl(const sInputParams& params, mfxIMPL& impl) {
    if (!params.bUseHWLib) {
        impl = MFX_IMPL_SOFTWARE;
        return MFX_ERR_NONE;
    }

#if (defined(_WIN64) || defined(_WIN32)) && (MFX_VERSION >= 1031) && (MFX_VERSION < 2000)
    mfxU32 num_adapters_available;

    mfxStatus sts = MFXQueryAdaptersNumber(&num_adapters_available);
    MSDK_CHECK_STATUS(sts, "MFXQueryAdaptersNumber failed");

    mfxBitstreamWrapper dummy_stream(8 * 1024 * 1024);

    std::vector<mfxAdapterInfo> displays_data(num_adapters_available);
    mfxAdaptersInfo adapters = { displays_data.data(), mfxU32(displays_data.size()), 0u };

    constexpr mfxU32 realloc_limit =
        4; // Limit for bitstream reallocation (it would be 2^realloc_limit times more than in beginning) before reporting an error in case if SPS couldn't be found
    for (mfxU32 i = 0;; ++i) {
        sts = m_FileReader->ReadNextFrame(&dummy_stream);

        if (MFX_ERR_MORE_DATA == sts && i < realloc_limit &&
            dummy_stream.MaxLength == dummy_stream.DataLength) {
            // Ignore the error now and alloc more data for bitstream

            dummy_stream.Extend(dummy_stream.MaxLength * 2);
        }
        MSDK_CHECK_STATUS(sts, "m_FileReader->ReadNextFrame failed");

        sts = MFXQueryAdaptersDecode(&dummy_stream, params.videoType, &adapters);

        if (MFX_ERR_MORE_DATA == sts) {
            continue;
        }

        if (sts == MFX_ERR_NOT_FOUND) {
            msdk_printf(MSDK_STRING("ERROR: No suitable adapters found for this workload\n"));
        }
        MSDK_CHECK_STATUS(sts, "MFXQueryAdapters failed");

        break;
    }

    m_FileReader->Reset();

    mfxU32 idx = GetPreferredAdapterNum(adapters, params);
    switch (adapters.Adapters[idx].Number) {
        case 0:
            impl = MFX_IMPL_HARDWARE;
            break;
        case 1:
            impl = MFX_IMPL_HARDWARE2;
            break;
        case 2:
            impl = MFX_IMPL_HARDWARE3;
            break;
        case 3:
            impl = MFX_IMPL_HARDWARE4;
            break;

        default:
            // Try searching on all display adapters
            impl = MFX_IMPL_HARDWARE_ANY;
            break;
    }
#else
    // Library should pick first available compatible adapter during InitEx call with MFX_IMPL_HARDWARE_ANY
    impl = MFX_IMPL_HARDWARE_ANY;
#endif // (defined(_WIN64) || defined(_WIN32)) && (MFX_VERSION >= 1031)

    // If d3d11 surfaces are used ask the library to run acceleration through D3D11
    // feature may be unsupported due to OS or MSDK API version

    if (D3D11_MEMORY == params.memType)
        impl |= MFX_IMPL_VIA_D3D11;

    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::Init(sInputParams* pParams) {
    MSDK_CHECK_POINTER(pParams, MFX_ERR_NULL_PTR);

    mfxStatus sts = MFX_ERR_NONE;

    // prepare input stream file reader
    // for VP8 complete and single frame reader is a requirement
    // create reader that supports completeframe mode for latency oriented scenarios
    if (pParams->bLowLat || pParams->bCalLat) {
        switch (pParams->videoType) {
            case MFX_CODEC_AVC:
                m_FileReader.reset(new CH264FrameReader());
                m_bIsCompleteFrame = true;
                m_bPrintLatency    = pParams->bCalLat;
                break;
            case MFX_CODEC_JPEG:
                m_FileReader.reset(new CJPEGFrameReader());
                m_bIsCompleteFrame = true;
                m_bPrintLatency    = pParams->bCalLat;
                break;
#if (MFX_VERSION < 2000)
            case MFX_CODEC_VP8:
#endif
            case MFX_CODEC_VP9:
            case MFX_CODEC_AV1:
                m_FileReader.reset(new CIVFFrameReader());
                m_bIsCompleteFrame = true;
                m_bPrintLatency    = pParams->bCalLat;
                break;
            default:
                return MFX_ERR_UNSUPPORTED; // latency mode is supported only for H.264 and JPEG codecs
        }
    }
    else {
        switch (pParams->videoType) {
#if (MFX_VERSION < 2000)
            case MFX_CODEC_VP8:
#endif
            case MFX_CODEC_VP9:
            case MFX_CODEC_AV1:
                m_FileReader.reset(new CIVFFrameReader());
                break;
            default:
                m_FileReader.reset(new CSmplBitstreamReader());
                break;
        }
    }

    if (pParams->fourcc)
        m_fourcc = pParams->fourcc;

#ifdef LIBVA_SUPPORT
    if (pParams->bPerfMode)
        m_bPerfMode = true;
#endif

    if (pParams->Width)
        m_vppOutWidth = pParams->Width;
    if (pParams->Height)
        m_vppOutHeight = pParams->Height;

#if defined(LINUX32) || defined(LINUX64)
    m_strDevicePath = pParams->strDevicePath;
#endif

    m_memType = pParams->memType;

    m_nMaxFps = pParams->nMaxFPS;
    m_nFrames = pParams->nFrames ? pParams->nFrames : MFX_INFINITE;

    m_bOutI420 = pParams->outI420;

    m_nTimeout        = pParams->nTimeout;
    m_bSoftRobustFlag = pParams->bSoftRobustFlag;

    // Initializing file reader
    totalBytesProcessed = 0;
    sts                 = m_FileReader->Init(pParams->strSrcFile);
    MSDK_CHECK_STATUS(sts, "m_FileReader->Init failed");

    mfxInitParamlWrap initPar;

#if (MFX_VERSION >= 2000)
    initPar.Version.Major = (pParams->bUseHWLib == true) ? 1 : 2;
    initPar.Version.Minor = 0;
#else
    // we set version to 1.0 and later we will query actual version of the library which will got leaded
    initPar.Version.Major = 1;
    initPar.Version.Minor = 0;
#endif

    initPar.GPUCopy = pParams->gpuCopy;

    if (pParams->nThreadsNum) {
        auto threadsPar       = initPar.AddExtBuffer<mfxExtThreadsParam>();
        threadsPar->NumThread = pParams->nThreadsNum;
    }
    if (pParams->SchedulingType) {
        auto threadsPar            = initPar.AddExtBuffer<mfxExtThreadsParam>();
        threadsPar->SchedulingType = pParams->SchedulingType;
    }
    if (pParams->Priority) {
        auto threadsPar      = initPar.AddExtBuffer<mfxExtThreadsParam>();
        threadsPar->Priority = pParams->Priority;
    }

    if (pParams->eDeinterlace) {
        m_diMode = pParams->eDeinterlace;
    }

    if (pParams->bUseFullColorRange) {
        m_bVppFullColorRange = pParams->bUseFullColorRange;
    }

    bool bResolutionSpecified =
        pParams->Width || pParams->Height; // potentially VPP can be inserted

    if (bResolutionSpecified)
        m_bDecOutSysmem = pParams->bUseHWLib ? false : true;
    else
        m_bDecOutSysmem = pParams->memType == SYSTEM_MEMORY;

    m_eWorkMode = pParams->mode;

    m_monitorType = pParams->monitorType;
    // create device and allocator
#if defined(LIBVA_SUPPORT)
    m_libvaBackend = pParams->libvaBackend;
#endif // defined(MFX_LIBVA_SUPPORT)

    sts = GetImpl(*pParams, initPar.Implementation);
    MSDK_CHECK_STATUS(sts, "GetImpl failed");

    if (pParams->api2xDispatcher) {
        // Initialize VPL session using 2.x smart dispatcher
        // and CLI choice of target implementation
        mfxVariant implValue;
        implValue.Type = MFX_VARIANT_TYPE_U32;
        if (!pParams->bUseHWLib) {
            implValue.Data.U32 = MFX_IMPL_TYPE_SOFTWARE;
        }
        else {
            implValue.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
        }
        m_mfxLoader   = MFXLoad();
        mfxConfig cfg = MFXCreateConfig(m_mfxLoader);
        sts           = MFXSetConfigFilterProperty(
            cfg,
            reinterpret_cast<mfxU8*>(const_cast<char*>("mfxImplDescription.Impl")),
            implValue);

        // call MFXSetConfigFilterProperty with correct AccelerationMode
        if (pParams->bUseHWLib)
            sts = VPL_SetAccelMode(m_mfxLoader, pParams->memType);

        mfxImplDescription* implDesc = nullptr;
        if (pParams->bUseAdapterNum) {
            mfxVariant adapterValue;
            cfg = MFXCreateConfig(m_mfxLoader);

            adapterValue.Type     = MFX_VARIANT_TYPE_U32;
            adapterValue.Data.U32 = pParams->adapterNum;
            sts                   = MFXSetConfigFilterProperty(
                cfg,
                reinterpret_cast<mfxU8*>(const_cast<char*>("mfxImplDescription.VendorImplID")),
                adapterValue);

            sts = MFXEnumImplementations(m_mfxLoader,
                                         0,
                                         MFX_IMPLCAPS_IMPLDESCSTRUCTURE,
                                         reinterpret_cast<mfxHDL*>(&implDesc));
            MSDK_CHECK_STATUS(sts, "MFXEnumImplementations failed");

            printf("\nmfxImplDescription for loaded impl:\n");
            printf("  AccelerationMode = 0x%08x\n", implDesc->AccelerationMode);
            printf("  VendorImplID     = 0x%08x\n", implDesc->VendorImplID);
            printf("  DeviceID         = %s\n", implDesc->Dev.DeviceID);
            printf("\n");
        }

        sts = MFXCreateSession(m_mfxLoader, 0, m_mfxSession.getSessionPtr());
        MSDK_CHECK_STATUS(sts, "MFXCreateSession failed");

        if (pParams->bUseAdapterNum)
            MFXDispReleaseImplDescription(m_mfxLoader, implDesc);
    }
    else {
        sts = m_mfxSession.InitEx(initPar);
        MSDK_CHECK_STATUS(sts, "m_mfxSession.InitEx failed");
    }

    mfxVersion version;
    sts = m_mfxSession.QueryVersion(&version); // get real API version of the loaded library
    MSDK_CHECK_STATUS(sts, "m_mfxSession.QueryVersion failed");

    sts = m_mfxSession.QueryIMPL(&m_impl); // get actual library implementation
    MSDK_CHECK_STATUS(sts, "m_mfxSession.QueryIMPL failed");

    bool isDeviceRequired = false;
    mfxHandleType hdl_t;
#if D3D_SURFACES_SUPPORT
    isDeviceRequired = m_memType != SYSTEM_MEMORY || !m_bDecOutSysmem;
    hdl_t =
    #if MFX_D3D11_SUPPORT
        D3D11_MEMORY == m_memType ? MFX_HANDLE_D3D11_DEVICE :
    #endif // #if MFX_D3D11_SUPPORT
                                  MFX_HANDLE_D3D9_DEVICE_MANAGER;
#elif LIBVA_SUPPORT
    #if (MFX_VERSION >= 2000)
    isDeviceRequired      = pParams->bUseHWLib;
    if (isDeviceRequired == true)
        hdl_t = MFX_HANDLE_VA_DISPLAY;
    #else
    isDeviceRequired = true; // on Linux MediaSDK doesn't create device internally
    hdl_t            = MFX_HANDLE_VA_DISPLAY;
    #endif
#endif
    if (isDeviceRequired) {
        sts = CreateHWDevice();
        MSDK_CHECK_STATUS(sts, "CreateHWDevice failed");
        mfxHDL hdl = NULL;
        sts        = m_hwdev->GetHandle(hdl_t, &hdl);
        MSDK_CHECK_STATUS(sts, "m_hwdev->GetHandle failed");
        sts = m_mfxSession.SetHandle(hdl_t, hdl);
        MSDK_CHECK_STATUS(sts, "m_mfxSession.SetHandle failed");
    }
#if (MFX_VERSION < 2000)
    if (pParams->bIsMVC && !CheckVersion(&version, MSDK_FEATURE_MVC)) {
        msdk_printf(MSDK_STRING("error: MVC is not supported in the %d.%d API version\n"),
                    version.Major,
                    version.Minor);
        return MFX_ERR_UNSUPPORTED;
    }

    if ((pParams->videoType == MFX_CODEC_JPEG) &&
        !CheckVersion(&version, MSDK_FEATURE_JPEG_DECODE)) {
        msdk_printf(MSDK_STRING("error: Jpeg is not supported in the %d.%d API version\n"),
                    version.Major,
                    version.Minor);
        return MFX_ERR_UNSUPPORTED;
    }
#endif
    if (pParams->bLowLat && !CheckVersion(&version, MSDK_FEATURE_LOW_LATENCY)) {
        msdk_printf(
            MSDK_STRING("error: Low Latency mode is not supported in the %d.%d API version\n"),
            version.Major,
            version.Minor);
        return MFX_ERR_UNSUPPORTED;
    }

    if (pParams->eDeinterlace && (pParams->eDeinterlace != MFX_DEINTERLACING_ADVANCED) &&
        (pParams->eDeinterlace != MFX_DEINTERLACING_BOB)) {
        msdk_printf(MSDK_STRING("error: Unsupported deinterlace value: %d\n"),
                    (int)pParams->eDeinterlace);
        return MFX_ERR_UNSUPPORTED;
    }

    if (pParams->bRenderWin) {
        m_bRenderWin = pParams->bRenderWin;
        // note: currently position is unsupported for Wayland
#if !defined(LIBVA_WAYLAND_SUPPORT)
        m_nRenderWinX = pParams->nRenderWinX;
        m_nRenderWinY = pParams->nRenderWinY;
#endif
    }

    m_delayTicks = pParams->nMaxFPS ? msdk_time_get_frequency() / pParams->nMaxFPS : 0;

    // create decoder
    m_pmfxDEC = new MFXVideoDECODE(m_mfxSession);
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_MEMORY_ALLOC);

#if (MFX_VERSION >= 2000)
    // 2.x internal memory
    m_bAPI2XInternalMem = pParams->api2xInternalMem;
    if (m_bAPI2XInternalMem == true) {
        // create internal memory allocator
        m_pmfxMemory = new MFXMemory(m_mfxSession);
        MSDK_CHECK_POINTER(m_pmfxMemory, MFX_ERR_MEMORY_ALLOC);
    }

    // enable fused decvpp in decode + vpp pipeline pathway
    m_bAPI2XDecVPP = pParams->api2xDecVPP;
    // simple way to get avg. fps for vpl performance comparison
    m_bAPI2XPerf = pParams->api2xPerf;
#endif

    // set video type in parameters
    m_mfxVideoParams.mfx.CodecId = pParams->videoType;
#if (MFX_VERSION < 2000)
    if (CheckVersion(&version, MSDK_FEATURE_PLUGIN_API)) {
        /* Here we actually define the following codec initialization scheme:
        *  1. If plugin path or guid is specified: we load user-defined plugin (example: VP8 sample decoder plugin)
        *  2. If plugin path not specified:
        *    2.a) we check if codec is distributed as a mediasdk plugin and load it if yes
        *    2.b) if codec is not in the list of mediasdk plugins, we assume, that it is supported inside mediasdk library
        */
        // Load user plug-in, should go after CreateAllocator function (when all callbacks were initialized)
        if (pParams->pluginParams.type == MFX_PLUGINLOAD_TYPE_FILE &&
            msdk_strnlen(pParams->pluginParams.strPluginPath,
                         sizeof(pParams->pluginParams.strPluginPath))) {
            m_pUserModule.reset(new MFXVideoUSER(m_mfxSession));
            if (pParams->videoType == MFX_CODEC_HEVC || pParams->videoType == MFX_CODEC_VP8 ||
                pParams->videoType == MFX_CODEC_VP9) {
                m_pPlugin.reset(
                    LoadPlugin(MFX_PLUGINTYPE_VIDEO_DECODE,
                               m_mfxSession,
                               pParams->pluginParams.pluginGuid,
                               1,
                               pParams->pluginParams.strPluginPath,
                               (mfxU32)msdk_strnlen(pParams->pluginParams.strPluginPath,
                                                    sizeof(pParams->pluginParams.strPluginPath))));
            }
            if (m_pPlugin.get() == NULL)
                sts = MFX_ERR_UNSUPPORTED;
        }
        else {
            bool isDefaultPlugin = false;
            if (AreGuidsEqual(pParams->pluginParams.pluginGuid, MSDK_PLUGINGUID_NULL)) {
                mfxIMPL impl = pParams->bUseHWLib ? MFX_IMPL_HARDWARE : MFX_IMPL_SOFTWARE;
                pParams->pluginParams.pluginGuid =
                    msdkGetPluginUID(impl, MSDK_VDECODE, pParams->videoType);
                isDefaultPlugin = true;
            }
            if (!AreGuidsEqual(pParams->pluginParams.pluginGuid, MSDK_PLUGINGUID_NULL)) {
                m_pPlugin.reset(LoadPlugin(MFX_PLUGINTYPE_VIDEO_DECODE,
                                           m_mfxSession,
                                           pParams->pluginParams.pluginGuid,
                                           1));
                if (m_pPlugin.get() == NULL)
                    sts = MFX_ERR_UNSUPPORTED;
            }
            if (sts == MFX_ERR_UNSUPPORTED) {
                msdk_printf(
                    MSDK_STRING("%s"),
                    isDefaultPlugin
                        ? MSDK_STRING(
                              "Default plugin cannot be loaded (possibly you have to define plugin explicitly)\n")
                        : MSDK_STRING("Explicitly specified plugin cannot be loaded.\n"));
            }
        }
        MSDK_CHECK_STATUS(sts, "Plugin load failed");
    }

    #if (MFX_VERSION >= 1034)
    m_mfxVideoParams.mfx.IgnoreLevelConstrain = pParams->bIgnoreLevelConstrain;
    #endif
#endif

    // Populate parameters. Involves DecodeHeader call
    sts = InitMfxParams(pParams);
    MSDK_CHECK_STATUS(sts, "InitMfxParams failed");

    if (m_bVppIsUsed) {
        m_pmfxVPP = new MFXVideoVPP(m_mfxSession);
        if (!m_pmfxVPP)
            return MFX_ERR_MEMORY_ALLOC;
    }

    if (m_eWorkMode == MODE_FILE_DUMP) {
        // prepare YUV file writer
        sts = m_FileWriter.Init(pParams->strDstFile, pParams->numViews);
        MSDK_CHECK_STATUS(sts, "m_FileWriter.Init failed");
    }
    else if ((m_eWorkMode != MODE_PERFORMANCE) && (m_eWorkMode != MODE_RENDERING)) {
        msdk_printf(MSDK_STRING("error: unsupported work mode\n"));
        return MFX_ERR_UNSUPPORTED;
    }

    sts = CreateAllocator();
    MSDK_CHECK_STATUS(sts, "CreateAllocator failed");

    // in case of HW accelerated decode frames must be allocated prior to decoder initialization
    sts = AllocFrames();
    MSDK_CHECK_STATUS(sts, "AllocFrames failed");

#if (MFX_VERSION >= 2000)
    if (m_bAPI2XDecVPP == true && m_bVppIsUsed == false) {
        MSDK_CHECK_STATUS(MFX_ERR_UNSUPPORTED,
                          "Option -api2x_decvpp needs proper -w and -h values for vpp operation");
    }

    if (m_bAPI2XDecVPP == true) {
        // create fused decodevpp instance
        m_pmfxDecVPP = new MFXVideoDECODE_VPP(m_mfxSession);
        MSDK_CHECK_POINTER(m_pmfxDecVPP, MFX_ERR_MEMORY_ALLOC);

        // enable only one vpp channel for now
        m_numVPPCh = 1;

        m_pmfxVPPChParams = new mfxVideoChannelParam[m_numVPPCh];
        for (mfxU16 i = 0; i < m_numVPPCh; i++)
            memset(&m_pmfxVPPChParams[i], 0, sizeof(mfxVideoChannelParam));

        MSDK_MEMCPY_BUF(&m_pmfxVPPChParams[0].VPP,
                        0,
                        sizeof(mfxFrameInfo),
                        &m_mfxVppVideoParams.vpp.Out,
                        sizeof(mfxFrameInfo));
        m_pmfxVPPChParams[0].VPP.ChannelId = 1;
        m_pmfxVPPChParams[0].Protected     = 0;
        m_pmfxVPPChParams[0].IOPattern =
            MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        m_pmfxVPPChParams[0].ExtParam    = NULL;
        m_pmfxVPPChParams[0].NumExtParam = 0;

        sts = m_pmfxDecVPP->Init(&m_mfxVideoParams, &m_pmfxVPPChParams, m_numVPPCh);
        MSDK_CHECK_STATUS(sts, "m_pmfxDecVPP.Init failed");
    }
    else {
#endif
        sts = m_pmfxDEC->Init(&m_mfxVideoParams);
        if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
            msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
            MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
        }
        MSDK_CHECK_STATUS(sts, "m_pmfxDEC->Init failed");

        if (m_bVppIsUsed) {
            if (m_diMode)
                m_mfxVppVideoParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

            if (pParams->ScalingMode) {
                auto par         = m_mfxVppVideoParams.AddExtBuffer<mfxExtVPPScaling>();
                par->ScalingMode = pParams->ScalingMode;
            }

            sts = m_pmfxVPP->Init(&m_mfxVppVideoParams);
            if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
                msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
                MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
            }
            MSDK_CHECK_STATUS(sts, "m_pmfxVPP->Init failed");
        }

        sts = m_pmfxDEC->GetVideoParam(&m_mfxVideoParams);
        MSDK_CHECK_STATUS(sts, "m_pmfxDEC->GetVideoParam failed");
#if (MFX_VERSION >= 2000)
    }
#endif

    if (m_eWorkMode == MODE_RENDERING) {
        sts = CreateRenderingWindow(pParams);
        MSDK_CHECK_STATUS(sts, "CreateRenderingWindow failed");
    }

    return sts;
}

bool CDecodingPipeline::IsVppRequired(sInputParams* pParams) {
    bool bVppIsUsed = false;
    /* Re-size */
    if ((m_mfxVideoParams.mfx.FrameInfo.CropW != pParams->Width) ||
        (m_mfxVideoParams.mfx.FrameInfo.CropH != pParams->Height)) {
        bVppIsUsed = pParams->Width && pParams->Height;
#if MFX_VERSION >= 1022
        if ((MODE_DECODER_POSTPROC_AUTO == pParams->nDecoderPostProcessing) ||
            (MODE_DECODER_POSTPROC_FORCE == pParams->nDecoderPostProcessing)) {
            /* Decoder will make decision about internal post-processing usage slightly later */
            bVppIsUsed = false;
        }
#endif //MFX_VERSION >= 1022
    }

    // JPEG and Capture decoders can provide output in nv12 and rgb4 formats
    if (pParams->videoType == MFX_CODEC_JPEG) {
        bVppIsUsed |= m_fourcc && (m_fourcc != MFX_FOURCC_NV12) && (m_fourcc != MFX_FOURCC_RGB4);
    }
    else {
        bVppIsUsed |= m_fourcc && (m_fourcc != m_mfxVideoParams.mfx.FrameInfo.FourCC);
    }

    if (pParams->eDeinterlace) {
        bVppIsUsed = true;
    }

    if ((MODE_DECODER_POSTPROC_AUTO == pParams->nDecoderPostProcessing) ||
        (MODE_DECODER_POSTPROC_FORCE == pParams->nDecoderPostProcessing) )
    {
        /* Decoder will make decision about internal post-processing usage slightly later */
        if((pParams->videoType == MFX_CODEC_AVC) || (pParams->videoType == MFX_CODEC_HEVC))
            bVppIsUsed = false;
    }

    return bVppIsUsed;
}

void CDecodingPipeline::Close() {
#if D3D_SURFACES_SUPPORT
    m_d3dRender.Close();
#endif
    MSDK_SAFE_DELETE(m_pmfxDEC);
    MSDK_SAFE_DELETE(m_pmfxVPP);

#if (MFX_VERSION >= 2000)
    if (m_bAPI2XInternalMem == true) {
        MSDK_SAFE_DELETE(m_pmfxMemory);
    }
    if (m_bAPI2XDecVPP == true) {
        MSDK_SAFE_DELETE(m_pmfxDecVPP);
        MSDK_SAFE_DELETE_ARRAY(m_pmfxVPPChParams);
    }
#endif

    DeleteFrames();

#if (MFX_VERSION < 2000)
    DeallocateExtMVCBuffers();

    m_pPlugin.reset();
#endif
    m_mfxSession.Close();
    if (m_mfxLoader)
        MFXUnload(m_mfxLoader);
    m_FileWriter.Close();
    if (m_FileReader.get())
        m_FileReader->Close();

    auto vppExtParams = m_mfxVppVideoParams.GetExtBuffer<mfxExtVPPDoNotUse>();
    if (vppExtParams)
        MSDK_SAFE_DELETE_ARRAY(vppExtParams->AlgList);

    // allocator if used as external for MediaSDK must be deleted after decoder
    DeleteAllocator();

    return;
}

mfxStatus CDecodingPipeline::CreateRenderingWindow(sInputParams* pParams) {
    mfxStatus sts = MFX_ERR_NONE;

#if D3D_SURFACES_SUPPORT
    sWindowParams windowParams;

    windowParams.lpWindowName = pParams->bWallNoTitle ? NULL : MSDK_STRING("sample_decode");
    windowParams.nx           = pParams->nWallW;
    windowParams.ny           = pParams->nWallH;
    if (m_bVppIsUsed) {
        windowParams.nWidth  = m_mfxVppVideoParams.vpp.Out.Width;
        windowParams.nHeight = m_mfxVppVideoParams.vpp.Out.Height;
    }
    else {
        windowParams.nWidth  = m_mfxVideoParams.mfx.FrameInfo.Width;
        windowParams.nHeight = m_mfxVideoParams.mfx.FrameInfo.Height;
    }

    windowParams.ncell    = pParams->nWallCell;
    windowParams.nAdapter = pParams->nWallMonitor;

    windowParams.lpClassName = MSDK_STRING("Render Window Class");
    windowParams.dwStyle     = WS_OVERLAPPEDWINDOW;
    windowParams.hWndParent  = NULL;
    windowParams.hMenu       = NULL;
    windowParams.hInstance   = GetModuleHandle(NULL);
    windowParams.lpParam     = NULL;
    windowParams.bFullScreen = FALSE;

    sts = m_d3dRender.Init(windowParams);
    MSDK_CHECK_STATUS(sts, "m_d3dRender.Init failed");

    //setting videowall flag
    m_bIsVideoWall = 0 != windowParams.nx;

#endif
    return sts;
}

mfxStatus CDecodingPipeline::InitMfxParams(sInputParams* pParams) {
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NULL_PTR);
    mfxStatus sts    = MFX_ERR_NONE;
    mfxU32& numViews = pParams->numViews;

#if (MFX_VERSION >= 1025)
    if (pParams->bErrorReport) {
        auto decErrorReport = m_mfxBS.AddExtBuffer<mfxExtDecodeErrorReport>();
        MSDK_CHECK_POINTER(decErrorReport, MFX_ERR_MEMORY_ALLOC);
    }
#endif

    if (m_mfxVideoParams.mfx.CodecId == MFX_CODEC_VP9)
        m_mfxVideoParams.mfx.EnableReallocRequest = MFX_CODINGOPTION_ON;

    // try to find a sequence header in the stream
    // if header is not found this function exits with error (e.g. if device was lost and there's no header in the remaining stream)
    for (;;) {
        // trying to find PicStruct information in AVI headers
        if (m_mfxVideoParams.mfx.CodecId == MFX_CODEC_JPEG)
            MJPEG_AVI_ParsePicStruct(&m_mfxBS);
#if (MFX_VERSION >= 1025)
        if (pParams->bErrorReport) {
            auto errorReport = m_mfxBS.GetExtBuffer<mfxExtDecodeErrorReport>();
            MSDK_CHECK_POINTER(errorReport, MFX_ERR_NOT_INITIALIZED);

            errorReport->ErrorTypes = 0;

            // parse bit stream and fill mfx params
            sts = m_pmfxDEC->DecodeHeader(&m_mfxBS, &m_mfxVideoParams);

            PrintDecodeErrorReport(errorReport);
        }
        else
#endif
        {
            // parse bit stream and fill mfx params
            sts = m_pmfxDEC->DecodeHeader(&m_mfxBS, &m_mfxVideoParams);
        }

        if (MFX_ERR_NONE == sts) {
            m_bVppIsUsed = IsVppRequired(pParams);
        }
#if (MFX_VERSION < 2000)
        if (MFX_ERR_NONE == sts && !(m_impl & MFX_IMPL_SOFTWARE) && // hw lib
            (m_mfxVideoParams.mfx.FrameInfo.BitDepthLuma == 10) && // hevc 10 bit
            (m_mfxVideoParams.mfx.CodecId == MFX_CODEC_HEVC) &&
            AreGuidsEqual(pParams->pluginParams.pluginGuid,
                          MFX_PLUGINID_HEVCD_SW) && // sw hevc decoder
            m_bVppIsUsed) {
            sts = MFX_ERR_UNSUPPORTED;
            msdk_printf(MSDK_STRING(
                "Error: Combination of (SW HEVC plugin in 10bit mode + HW lib VPP) isn't supported. Use -sw option.\n"));
        }
        if (m_pPlugin.get() && pParams->videoType == CODEC_VP8 && MFX_ERR_NONE == sts) {
            // force set format to nv12 as the vp8 plugin uses yv12
            m_mfxVideoParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        }
#endif
        if (MFX_ERR_MORE_DATA == sts) {
            if (m_mfxBS.MaxLength == m_mfxBS.DataLength) {
                m_mfxBS.Extend(m_mfxBS.MaxLength * 2);
            }
            // read a portion of data
            totalBytesProcessed += m_mfxBS.DataOffset;
            sts = m_FileReader->ReadNextFrame(&m_mfxBS);
            MSDK_CHECK_STATUS(sts, "m_FileReader->ReadNextFrame failed");

            continue;
        }
        else {
#if (MFX_VERSION < 2000)
            // Enter MVC mode
            if (m_bIsMVC) {
                // Check for attached external parameters - if we have them already,
                // we don't need to attach them again
                if (NULL != m_mfxVideoParams.ExtParam)
                    break;

                auto mvcSeqDesc = m_mfxVideoParams.AddExtBuffer<mfxExtMVCSeqDesc>();
                MSDK_CHECK_POINTER(mvcSeqDesc, MFX_ERR_MEMORY_ALLOC);

                sts = m_pmfxDEC->DecodeHeader(&m_mfxBS, &m_mfxVideoParams);

                if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
                    sts = AllocateExtMVCBuffers();

                    MSDK_CHECK_STATUS(sts, "AllocateExtMVCBuffers failed");
                    MSDK_CHECK_POINTER(m_mfxVideoParams.ExtParam, MFX_ERR_MEMORY_ALLOC);
                    continue;
                }
            }
#endif
            // if input is interlaced JPEG stream
            if (m_mfxBS.PicStruct == MFX_PICSTRUCT_FIELD_TFF ||
                m_mfxBS.PicStruct == MFX_PICSTRUCT_FIELD_BFF) {
                m_mfxVideoParams.mfx.FrameInfo.CropH *= 2;
                m_mfxVideoParams.mfx.FrameInfo.Height =
                    MSDK_ALIGN16(m_mfxVideoParams.mfx.FrameInfo.CropH);
                m_mfxVideoParams.mfx.FrameInfo.PicStruct = m_mfxBS.PicStruct;
            }

            switch (pParams->nRotation) {
                case 0:
                    m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_0;
                    break;
                case 90:
                    m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_90;
                    break;
                case 180:
                    m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_180;
                    break;
                case 270:
                    m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_270;
                    break;
                default:
                    return MFX_ERR_UNSUPPORTED;
            }

            break;
        }
    }

    // check DecodeHeader status
    if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
        msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
        MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    }
    MSDK_CHECK_STATUS(sts, "m_pmfxDEC->DecodeHeader failed");

    if (!m_mfxVideoParams.mfx.FrameInfo.FrameRateExtN ||
        !m_mfxVideoParams.mfx.FrameInfo.FrameRateExtD) {
        msdk_printf(MSDK_STRING("pretending that stream is 30fps one\n"));
        m_mfxVideoParams.mfx.FrameInfo.FrameRateExtN = 30;
        m_mfxVideoParams.mfx.FrameInfo.FrameRateExtD = 1;
    }
    if (!m_mfxVideoParams.mfx.FrameInfo.AspectRatioW ||
        !m_mfxVideoParams.mfx.FrameInfo.AspectRatioH) {
        msdk_printf(MSDK_STRING("pretending that aspect ratio is 1:1\n"));
        m_mfxVideoParams.mfx.FrameInfo.AspectRatioW = 1;
        m_mfxVideoParams.mfx.FrameInfo.AspectRatioH = 1;
    }

    // Videoparams for RGB4 JPEG decoder output
    if ((pParams->fourcc == MFX_FOURCC_RGB4) && (pParams->videoType == MFX_CODEC_JPEG)) {
        m_mfxVideoParams.mfx.FrameInfo.FourCC       = MFX_FOURCC_RGB4;
        m_mfxVideoParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
        if (pParams->chromaType == MFX_JPEG_COLORFORMAT_RGB) {
            m_mfxVideoParams.mfx.JPEGColorFormat = pParams->chromaType;
        }
    }

    // specify memory type
    if (!m_bVppIsUsed)
        m_mfxVideoParams.IOPattern =
            (mfxU16)(m_memType != SYSTEM_MEMORY ? MFX_IOPATTERN_OUT_VIDEO_MEMORY
                                                : MFX_IOPATTERN_OUT_SYSTEM_MEMORY);
    else
        m_mfxVideoParams.IOPattern = (mfxU16)(pParams->bUseHWLib ? MFX_IOPATTERN_OUT_VIDEO_MEMORY
                                                                 : MFX_IOPATTERN_OUT_SYSTEM_MEMORY);

#if MFX_VERSION >= 1022
    /* Lets make final decision how to use VPP...*/
    // if (!m_bVppIsUsed) {
        if ((m_mfxVideoParams.mfx.FrameInfo.CropW != pParams->Width && pParams->Width) ||
            (m_mfxVideoParams.mfx.FrameInfo.CropH != pParams->Height && pParams->Height) ||
            (pParams->nDecoderPostProcessing && pParams->videoType == MFX_CODEC_AVC) ||
            (pParams->nDecoderPostProcessing && pParams->videoType == MFX_CODEC_HEVC) ||
            (pParams->nDecoderPostProcessing && pParams->videoType == MFX_CODEC_JPEG &&
             pParams->fourcc == MFX_FOURCC_RGB4 &&
             // No need to use decoder's post processing for decoding of JPEG with RGB 4:4:4
             // to MFX_FOURCC_RGB4, because this decoding is done in one step
             // In every other case, color conversion is required, so try decoder's post processing.
             !(m_mfxVideoParams.mfx.JPEGColorFormat == MFX_JPEG_COLORFORMAT_RGB &&
               m_mfxVideoParams.mfx.FrameInfo.ChromaFormat == MFX_CHROMAFORMAT_YUV444))) {
            /* By default VPP used for resize */
            m_bVppIsUsed = true;
            /* But... lets try to use decoder's post processing */
            if (((MODE_DECODER_POSTPROC_AUTO == pParams->nDecoderPostProcessing) ||
                 (MODE_DECODER_POSTPROC_FORCE == pParams->nDecoderPostProcessing)) &&
                (MFX_CODEC_AVC == m_mfxVideoParams.mfx.CodecId ||
                 MFX_CODEC_HEVC == m_mfxVideoParams.mfx.CodecId ||
                 MFX_CODEC_JPEG == m_mfxVideoParams.mfx.CodecId) && /* Only for AVC, HEVC and JPEG */
                (MFX_PICSTRUCT_PROGRESSIVE ==
                 m_mfxVideoParams.mfx.FrameInfo.PicStruct)) /* ...And only for progressive!*/
            { /* it is possible to use decoder's post-processing */

                // JPEG only suppoted w/o resize, so use W/H from DecodeHeader(), if they are not set
                if (MFX_CODEC_JPEG == m_mfxVideoParams.mfx.CodecId &&
                    (!pParams->Width || !pParams->Height)) {
                    pParams->Width  = m_mfxVideoParams.mfx.FrameInfo.CropW;
                    pParams->Height = m_mfxVideoParams.mfx.FrameInfo.CropH;
                }

                // m_bVppIsUsed           = false;
                m_bVppIsUsed           = true;
                auto decPostProcessing = m_mfxVideoParams.AddExtBuffer<mfxExtDecVideoProcessing>();
                MSDK_CHECK_POINTER(decPostProcessing, MFX_ERR_MEMORY_ALLOC);

                decPostProcessing->In.CropX = 0;
                decPostProcessing->In.CropY = 0;
                decPostProcessing->In.CropW = m_mfxVideoParams.mfx.FrameInfo.CropW;
                decPostProcessing->In.CropH = m_mfxVideoParams.mfx.FrameInfo.CropH;

                decPostProcessing->Out.FourCC       = m_mfxVideoParams.mfx.FrameInfo.FourCC;
                decPostProcessing->Out.ChromaFormat = m_mfxVideoParams.mfx.FrameInfo.ChromaFormat;
                if ((pParams->fourcc == MFX_FOURCC_RGB4) &&
                    (pParams->videoType == MFX_CODEC_AVC || pParams->videoType == MFX_CODEC_HEVC))
                {
                    decPostProcessing->Out.FourCC = MFX_FOURCC_RGB4;
                    decPostProcessing->Out.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
                }
                decPostProcessing->Out.Width        = MSDK_ALIGN16(pParams->Width);
                decPostProcessing->Out.Height       = MSDK_ALIGN16(pParams->Height);
                decPostProcessing->Out.CropX        = 0;
                decPostProcessing->Out.CropY        = 0;
                decPostProcessing->Out.CropW        = pParams->Width;
                decPostProcessing->Out.CropH        = pParams->Height;

                msdk_printf(MSDK_STRING("Decoder's post-processing is used for resizing\n"));
            }
            /* POSTPROC_FORCE */
            // if (MODE_DECODER_POSTPROC_FORCE == pParams->nDecoderPostProcessing && m_bVppIsUsed) {
            //     /* it is impossible to use decoder's post-processing */
            //     msdk_printf(MSDK_STRING(
            //         "ERROR: decoder postprocessing (-dec_postproc forced) cannot be used for this stream!\n"));
            //     return MFX_ERR_UNSUPPORTED;
            // }
            // if ((m_bVppIsUsed) && (MODE_DECODER_POSTPROC_AUTO == pParams->nDecoderPostProcessing))
            //     msdk_printf(MSDK_STRING(
            //         "Decoder post-processing is unsupported for this stream, VPP is used\n"));
        }
    // }
#endif //MFX_VERSION >= 1022

#if (MFX_VERSION < 2000)
    // If MVC mode we need to detect number of views in stream
    if (m_bIsMVC) {
        auto sequenceBuffer = m_mfxVideoParams.GetExtBuffer<mfxExtMVCSeqDesc>();
        MSDK_CHECK_POINTER(sequenceBuffer, MFX_ERR_INVALID_VIDEO_PARAM);

        mfxU32 i = 0;
        numViews = 0;
        for (i = 0; i < sequenceBuffer->NumView; ++i) {
            /* Some MVC streams can contain different information about
               number of views and view IDs, e.x. numVews = 2
               and ViewId[0, 1] = 0, 2 instead of ViewId[0, 1] = 0, 1.
               numViews should be equal (max(ViewId[i]) + 1)
               to prevent crashes during output files writing */
            if (sequenceBuffer->View[i].ViewId >= numViews)
                numViews = sequenceBuffer->View[i].ViewId + 1;
        }
    }
    else {
        numViews = 1;
    }
#else
    numViews      = 1;
#endif
    m_mfxVideoParams.AsyncDepth = pParams->nAsyncDepth;

    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::InitVppFilters() {
    auto vppExtParams = m_mfxVppVideoParams.AddExtBuffer<mfxExtVPPDoNotUse>();
    MSDK_CHECK_POINTER(vppExtParams, MFX_ERR_MEMORY_ALLOC);

    vppExtParams->NumAlg = 4;

    /* In case of Reset() this code called twice!
     * But required to have only one allocation to prevent memleaks
     * Deallocation done in Close() */
    if (NULL == vppExtParams->AlgList)
        vppExtParams->AlgList = new mfxU32[vppExtParams->NumAlg];

    if (!vppExtParams->AlgList)
        return MFX_ERR_NULL_PTR;

    vppExtParams->AlgList[0] = MFX_EXTBUFF_VPP_DENOISE; // turn off denoising (on by default)
    vppExtParams->AlgList[1] =
        MFX_EXTBUFF_VPP_SCENE_ANALYSIS; // turn off scene analysis (on by default)
    vppExtParams->AlgList[2] =
        MFX_EXTBUFF_VPP_DETAIL; // turn off detail enhancement (on by default)
    vppExtParams->AlgList[3] =
        MFX_EXTBUFF_VPP_PROCAMP; // turn off processing amplified (on by default)

    if (m_diMode) {
        auto vppDi = m_mfxVppVideoParams.AddExtBuffer<mfxExtVPPDeinterlacing>();
        MSDK_CHECK_POINTER(vppDi, MFX_ERR_MEMORY_ALLOC);

        vppDi->Mode = m_diMode;
    }

    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::InitVppParams() {
    m_mfxVppVideoParams.IOPattern =
        (mfxU16)(m_bDecOutSysmem ? MFX_IOPATTERN_IN_SYSTEM_MEMORY : MFX_IOPATTERN_IN_VIDEO_MEMORY);

    m_mfxVppVideoParams.IOPattern |= (m_memType != SYSTEM_MEMORY) ? MFX_IOPATTERN_OUT_VIDEO_MEMORY
                                                                  : MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

    MSDK_MEMCPY_VAR(m_mfxVppVideoParams.vpp.In,
                    &m_mfxVideoParams.mfx.FrameInfo,
                    sizeof(mfxFrameInfo));

    m_mfxVppVideoParams.vpp.In.CropW = 3480;
    m_mfxVppVideoParams.vpp.In.Width = MSDK_ALIGN16(3480);
    m_mfxVppVideoParams.vpp.In.CropH = 2160;
    m_mfxVppVideoParams.vpp.In.Height = 2160;
    m_mfxVppVideoParams.vpp.In.FourCC = MFX_FOURCC_RGB4;

    MSDK_MEMCPY_VAR(m_mfxVppVideoParams.vpp.Out, &m_mfxVppVideoParams.vpp.In, sizeof(mfxFrameInfo));

    if (m_fourcc) {
        m_mfxVppVideoParams.vpp.Out.FourCC = m_fourcc;
    }

    if (m_vppOutWidth && m_vppOutHeight) {
        // m_mfxVppVideoParams.vpp.Out.CropW = m_vppOutWidth;
        // m_mfxVppVideoParams.vpp.Out.Width = MSDK_ALIGN16(m_vppOutWidth);
        // m_mfxVppVideoParams.vpp.Out.CropH = m_vppOutHeight;
        // m_mfxVppVideoParams.vpp.Out.Height =
        //     (MFX_PICSTRUCT_PROGRESSIVE == m_mfxVppVideoParams.vpp.Out.PicStruct)
        //         ? MSDK_ALIGN16(m_vppOutHeight)
        //         : MSDK_ALIGN32(m_vppOutHeight);
        m_mfxVppVideoParams.vpp.Out.CropW = 304;
        m_mfxVppVideoParams.vpp.Out.Width = MSDK_ALIGN16(304);
        m_mfxVppVideoParams.vpp.Out.CropH = 304;
        m_mfxVppVideoParams.vpp.Out.Height =
            (MFX_PICSTRUCT_PROGRESSIVE == m_mfxVppVideoParams.vpp.Out.PicStruct)
                ? MSDK_ALIGN16(304)
                : MSDK_ALIGN32(304);
#if (MFX_VERSION >= 2000)
        if (m_impl & MFX_IMPL_SOFTWARE) { // for vpl
            m_mfxVppVideoParams.vpp.Out.Height = m_vppOutHeight;
        }
#endif
    }

    m_mfxVppVideoParams.AsyncDepth = m_mfxVideoParams.AsyncDepth;

    m_VppSurfaceExtParams.clear();
    if (m_bVppFullColorRange) {
        //Let MSDK figure out the transfer matrix to use
        m_VppVideoSignalInfo.TransferMatrix = MFX_TRANSFERMATRIX_UNKNOWN;
        m_VppVideoSignalInfo.NominalRange   = MFX_NOMINALRANGE_0_255;

        m_VppSurfaceExtParams.push_back((mfxExtBuffer*)&m_VppVideoSignalInfo);
    }

    // P010 video surfaces should be shifted
    if (m_memType != SYSTEM_MEMORY && (m_mfxVppVideoParams.vpp.Out.FourCC == MFX_FOURCC_P010
#if (MFX_VERSION >= 1027)
                                       || m_mfxVppVideoParams.vpp.Out.FourCC == MFX_FOURCC_Y210
#endif
#if (MFX_VERSION >= 1031)
                                       || m_mfxVppVideoParams.vpp.Out.FourCC == MFX_FOURCC_P016 ||
                                       m_mfxVppVideoParams.vpp.Out.FourCC == MFX_FOURCC_Y216 ||
                                       m_mfxVppVideoParams.vpp.Out.FourCC == MFX_FOURCC_Y416
#endif
                                       )) {
        m_mfxVppVideoParams.vpp.Out.Shift = 1;
    }

    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::CreateHWDevice() {
#if D3D_SURFACES_SUPPORT
    mfxStatus sts = MFX_ERR_NONE;

    HWND window = NULL;
    bool render = (m_eWorkMode == MODE_RENDERING);

    if (render) {
        window = (D3D11_MEMORY == m_memType) ? NULL : m_d3dRender.GetWindowHandle();
    }

    #if MFX_D3D11_SUPPORT
    if (D3D11_MEMORY == m_memType)
        m_hwdev = new CD3D11Device();
    else
    #endif // #if MFX_D3D11_SUPPORT
        m_hwdev = new CD3D9Device();

    if (NULL == m_hwdev)
        return MFX_ERR_MEMORY_ALLOC;

    sts = m_hwdev->Init(window,
                        render ? (m_bIsMVC ? 2 : 1) : 0,
                        MSDKAdapter::GetNumber(m_mfxSession));
    MSDK_CHECK_STATUS(sts, "m_hwdev->Init failed");

    if (render)
        m_d3dRender.SetHWDevice(m_hwdev);
#elif LIBVA_SUPPORT
    mfxStatus sts = MFX_ERR_NONE;
    m_hwdev       = CreateVAAPIDevice(m_strDevicePath, m_libvaBackend);

    if (NULL == m_hwdev) {
        return MFX_ERR_MEMORY_ALLOC;
    }

    sts = m_hwdev->Init(&m_monitorType,
                        (m_eWorkMode == MODE_RENDERING) ? 1 : 0,
                        MSDKAdapter::GetNumber(m_mfxSession));
    MSDK_CHECK_STATUS(sts, "m_hwdev->Init failed");

    #if defined(LIBVA_WAYLAND_SUPPORT)
    if (m_eWorkMode == MODE_RENDERING && m_libvaBackend == MFX_LIBVA_WAYLAND) {
        CVAAPIDeviceWayland* w_dev = dynamic_cast<CVAAPIDeviceWayland*>(m_hwdev);
        if (!w_dev) {
            MSDK_CHECK_STATUS(MFX_ERR_DEVICE_FAILED, "Failed to reach Wayland VAAPI device");
        }
        Wayland* wld = w_dev->GetWaylandHandle();
        if (!wld) {
            MSDK_CHECK_STATUS(MFX_ERR_DEVICE_FAILED, "Failed to reach Wayland VAAPI device");
        }

        wld->SetRenderWinPos(m_nRenderWinX, m_nRenderWinY);
        wld->SetPerfMode(m_bPerfMode);
    }
    #endif //LIBVA_WAYLAND_SUPPORT

#endif
    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::ResetDevice() {
    if (m_hwdev)
        return m_hwdev->Reset();

    return CreateHWDevice();
}

mfxStatus CDecodingPipeline::AllocFrames() {
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NULL_PTR);

    mfxStatus sts = MFX_ERR_NONE;

    mfxFrameAllocRequest Request;
    mfxFrameAllocRequest VppRequest[2];

    mfxU16 nSurfNum    = 0; // number of surfaces for decoder
    mfxU16 nVppSurfNum = 0; // number of surfaces for vpp

    MSDK_ZERO_MEMORY(Request);

    MSDK_ZERO_MEMORY(VppRequest[0]);
    MSDK_ZERO_MEMORY(VppRequest[1]);

    sts = m_pmfxDEC->Query(&m_mfxVideoParams, &m_mfxVideoParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    MSDK_CHECK_STATUS(sts, "m_pmfxDEC->Query failed");

    // calculate number of surfaces required for decoder
    sts = m_pmfxDEC->QueryIOSurf(&m_mfxVideoParams, &Request);
    if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
        msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
        MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
        m_bDecOutSysmem = true;
    }
    MSDK_CHECK_STATUS(sts, "m_pmfxDEC->QueryIOSurf failed");

    if (m_nMaxFps) {
        // Add surfaces for rendering smoothness
        Request.NumFrameSuggested += m_nMaxFps / 3;
    }

    if (m_bVppIsUsed) {
        // respecify memory type between Decoder and VPP
        m_mfxVideoParams.IOPattern = (mfxU16)(m_bDecOutSysmem ? MFX_IOPATTERN_OUT_SYSTEM_MEMORY
                                                              : MFX_IOPATTERN_OUT_VIDEO_MEMORY);

        // recalculate number of surfaces required for decoder
        sts = m_pmfxDEC->QueryIOSurf(&m_mfxVideoParams, &Request);
        MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
        MSDK_CHECK_STATUS(sts, "m_pmfxDEC->QueryIOSurf failed");

        sts = InitVppParams();
        MSDK_CHECK_STATUS(sts, "InitVppParams failed");

        sts = m_pmfxVPP->Query(&m_mfxVppVideoParams, &m_mfxVppVideoParams);
        MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
        MSDK_CHECK_STATUS(sts, "m_pmfxVPP->Query failed");

        // VppRequest[0] for input frames request, VppRequest[1] for output frames request
        sts = m_pmfxVPP->QueryIOSurf(&m_mfxVppVideoParams, VppRequest);
        if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
            msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
            MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
        }
        MSDK_CHECK_STATUS(sts, "m_pmfxVPP->QueryIOSurf failed");

        if ((VppRequest[0].NumFrameSuggested < m_mfxVppVideoParams.AsyncDepth) ||
            (VppRequest[1].NumFrameSuggested < m_mfxVppVideoParams.AsyncDepth))
            return MFX_ERR_MEMORY_ALLOC;

        // If surfaces are shared by 2 components, c1 and c2. NumSurf = c1_out + c2_in - AsyncDepth + 1
        // The number of surfaces shared by vpp input and decode output
        nSurfNum = Request.NumFrameSuggested + VppRequest[0].NumFrameSuggested -
                   m_mfxVideoParams.AsyncDepth + 1;

        // The number of surfaces for vpp output.
        // Need to add one more surface in render mode if AsyncDepth == 1
        nVppSurfNum = VppRequest[1].NumFrameSuggested +
                      (m_eWorkMode == MODE_RENDERING ? m_mfxVideoParams.AsyncDepth == 1 : 0);

        // prepare allocation request
        Request.NumFrameSuggested = Request.NumFrameMin = nSurfNum;

        // surfaces are shared between vpp input and decode output
        Request.Type =
            MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE | MFX_MEMTYPE_FROM_VPPIN;
    }

    if ((Request.NumFrameSuggested < m_mfxVideoParams.AsyncDepth) &&
        (m_impl & MFX_IMPL_HARDWARE_ANY))
        return MFX_ERR_MEMORY_ALLOC;

    Request.Type |=
        (m_bDecOutSysmem) ? MFX_MEMTYPE_SYSTEM_MEMORY : MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

#ifdef LIBVA_SUPPORT
    if (!m_bVppIsUsed && (m_export_mode != vaapiAllocatorParams::DONOT_EXPORT)) {
        Request.Type |= MFX_MEMTYPE_EXPORT_FRAME;
    }
#endif

    // alloc frames for decoder
    sts = m_pGeneralAllocator->Alloc(m_pGeneralAllocator->pthis, &Request, &m_mfxResponse);
    MSDK_CHECK_STATUS(sts, "m_pGeneralAllocator->Alloc failed");

    if (m_bVppIsUsed) {
        // alloc frames for VPP
#ifdef LIBVA_SUPPORT
        if (m_export_mode != vaapiAllocatorParams::DONOT_EXPORT) {
            VppRequest[1].Type |= MFX_MEMTYPE_EXPORT_FRAME;
        }
#endif
        VppRequest[1].NumFrameSuggested = VppRequest[1].NumFrameMin = nVppSurfNum;
        MSDK_MEMCPY_VAR(VppRequest[1].Info, &(m_mfxVppVideoParams.vpp.Out), sizeof(mfxFrameInfo));

        sts = m_pGeneralAllocator->Alloc(m_pGeneralAllocator->pthis,
                                         &VppRequest[1],
                                         &m_mfxVppResponse);
        MSDK_CHECK_STATUS(sts, "m_pGeneralAllocator->Alloc failed");

        // prepare mfxFrameSurface1 array for decoder
        nVppSurfNum = m_mfxVppResponse.NumFrameActual;

        // AllocVppBuffers should call before AllocBuffers to set the value of m_OutputSurfacesNumber
        sts = AllocVppBuffers(nVppSurfNum);
        MSDK_CHECK_STATUS(sts, "AllocVppBuffers failed");
    }

    // prepare mfxFrameSurface1 array for decoder
    nSurfNum = m_mfxResponse.NumFrameActual;

    sts = AllocBuffers(nSurfNum);
    MSDK_CHECK_STATUS(sts, "AllocBuffers failed");

    for (int i = 0; i < nSurfNum; i++) {
        // initating each frame:
        MSDK_MEMCPY_VAR(m_pSurfaces[i].frame.Info, &(Request.Info), sizeof(mfxFrameInfo));
        m_pSurfaces[i].frame.Data.MemType = Request.Type;
        if (m_bExternalAlloc) {
            m_pSurfaces[i].frame.Data.MemId = m_mfxResponse.mids[i];
            if (m_bVppFullColorRange) {
                m_pSurfaces[i].frame.Data.ExtParam    = &m_VppSurfaceExtParams[0];
                m_pSurfaces[i].frame.Data.NumExtParam = (mfxU16)m_VppSurfaceExtParams.size();
            }
        }
    }

    // prepare mfxFrameSurface1 array for VPP
    for (int i = 0; i < nVppSurfNum; i++) {
        MSDK_MEMCPY_VAR(m_pVppSurfaces[i].frame.Info, &(VppRequest[1].Info), sizeof(mfxFrameInfo));
        if (m_bExternalAlloc) {
            m_pVppSurfaces[i].frame.Data.MemId = m_mfxVppResponse.mids[i];
            if (m_bVppFullColorRange) {
                m_pVppSurfaces[i].frame.Data.ExtParam    = &m_VppSurfaceExtParams[0];
                m_pVppSurfaces[i].frame.Data.NumExtParam = (mfxU16)m_VppSurfaceExtParams.size();
            }
        }
    }
    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::ReallocCurrentSurface(const mfxFrameInfo& info) {
    mfxStatus sts   = MFX_ERR_NONE;
    mfxMemId inMid  = nullptr;
    mfxMemId outMid = nullptr;

    if (!m_pGeneralAllocator)
        return MFX_ERR_MEMORY_ALLOC;

    m_pCurrentFreeSurface->frame.Info.CropW = info.CropW;
    m_pCurrentFreeSurface->frame.Info.CropH = info.CropH;
    if (info.Width > m_mfxVideoParams.mfx.FrameInfo.Width) {
        m_mfxVideoParams.mfx.FrameInfo.Width = MSDK_ALIGN16(info.Width);
    }
    else {
        m_mfxVideoParams.mfx.FrameInfo.Width = MSDK_ALIGN16(m_mfxVideoParams.mfx.FrameInfo.Width);
    }

    if (info.Height > m_mfxVideoParams.mfx.FrameInfo.Height) {
        m_mfxVideoParams.mfx.FrameInfo.Height = MSDK_ALIGN16(info.Height);
    }
    else {
        m_mfxVideoParams.mfx.FrameInfo.Height = MSDK_ALIGN16(m_mfxVideoParams.mfx.FrameInfo.Height);
    }

    m_pCurrentFreeSurface->frame.Info.Width  = m_mfxVideoParams.mfx.FrameInfo.Width;
    m_pCurrentFreeSurface->frame.Info.Height = m_mfxVideoParams.mfx.FrameInfo.Height;

    inMid = m_pCurrentFreeSurface->frame.Data.MemId;

    sts = m_pGeneralAllocator->ReallocFrame(inMid,
                                            &m_pCurrentFreeSurface->frame.Info,
                                            m_pCurrentFreeSurface->frame.Data.MemType,
                                            &outMid);
    if (MFX_ERR_NONE == sts)
        m_pCurrentFreeSurface->frame.Data.MemId = outMid;

    return sts;
}

mfxStatus CDecodingPipeline::CreateAllocator() {
    mfxStatus sts = MFX_ERR_NONE;

    m_pGeneralAllocator = new GeneralAllocator();
    if (m_memType != SYSTEM_MEMORY || !m_bDecOutSysmem) {
#if D3D_SURFACES_SUPPORT
        mfxHDL hdl = NULL;
        mfxHandleType hdl_t =
    #if MFX_D3D11_SUPPORT
            D3D11_MEMORY == m_memType ? MFX_HANDLE_D3D11_DEVICE :
    #endif // #if MFX_D3D11_SUPPORT
                                      MFX_HANDLE_D3D9_DEVICE_MANAGER;

        sts = m_hwdev->GetHandle(hdl_t, &hdl);
        MSDK_CHECK_STATUS(sts, "m_hwdev->GetHandle failed");

        // create D3D allocator
    #if MFX_D3D11_SUPPORT
        if (D3D11_MEMORY == m_memType) {
            D3D11AllocatorParams* pd3dAllocParams = new D3D11AllocatorParams;
            MSDK_CHECK_POINTER(pd3dAllocParams, MFX_ERR_MEMORY_ALLOC);
            pd3dAllocParams->pDevice = reinterpret_cast<ID3D11Device*>(hdl);

            m_pmfxAllocatorParams = pd3dAllocParams;
        }
        else
    #endif // #if MFX_D3D11_SUPPORT
        {
            D3DAllocatorParams* pd3dAllocParams = new D3DAllocatorParams;
            MSDK_CHECK_POINTER(pd3dAllocParams, MFX_ERR_MEMORY_ALLOC);
            pd3dAllocParams->pManager = reinterpret_cast<IDirect3DDeviceManager9*>(hdl);

            m_pmfxAllocatorParams = pd3dAllocParams;
        }

        /* In case of video memory we must provide MediaSDK with external allocator
        thus we demonstrate "external allocator" usage model.
        Call SetAllocator to pass allocator to mediasdk */
        sts = m_mfxSession.SetFrameAllocator(m_pGeneralAllocator);
        MSDK_CHECK_STATUS(sts, "m_mfxSession.SetFrameAllocator failed");

        m_bExternalAlloc = true;
#elif LIBVA_SUPPORT
        VADisplay va_dpy = NULL;
        sts              = m_hwdev->GetHandle(MFX_HANDLE_VA_DISPLAY, (mfxHDL*)&va_dpy);
        MSDK_CHECK_STATUS(sts, "m_hwdev->GetHandle failed");

        vaapiAllocatorParams* p_vaapiAllocParams = new vaapiAllocatorParams;
        MSDK_CHECK_POINTER(p_vaapiAllocParams, MFX_ERR_MEMORY_ALLOC);

        p_vaapiAllocParams->m_dpy = va_dpy;
        if (m_eWorkMode == MODE_RENDERING) {
            if (m_libvaBackend == MFX_LIBVA_DRM_MODESET) {
    #if defined(LIBVA_DRM_SUPPORT)
                CVAAPIDeviceDRM* drmdev           = dynamic_cast<CVAAPIDeviceDRM*>(m_hwdev);
                p_vaapiAllocParams->m_export_mode = vaapiAllocatorParams::CUSTOM_FLINK;
                p_vaapiAllocParams->m_exporter =
                    dynamic_cast<vaapiAllocatorParams::Exporter*>(drmdev->getRenderer());
    #endif
            }
            else if (m_libvaBackend == MFX_LIBVA_WAYLAND || m_libvaBackend == MFX_LIBVA_X11) {
                p_vaapiAllocParams->m_export_mode = vaapiAllocatorParams::PRIME;
            }
        }
        m_export_mode         = p_vaapiAllocParams->m_export_mode;
        m_pmfxAllocatorParams = p_vaapiAllocParams;

        /* In case of video memory we must provide MediaSDK with external allocator
        thus we demonstrate "external allocator" usage model.
        Call SetAllocator to pass allocator to mediasdk */
        sts = m_mfxSession.SetFrameAllocator(m_pGeneralAllocator);
        MSDK_CHECK_STATUS(sts, "m_mfxSession.SetFrameAllocator failed");

        m_bExternalAlloc = true;
#endif
    }
    else {
#if (MFX_VERSION >= 2000)
        if (m_bAPI2XInternalMem == false && m_bAPI2XDecVPP == false)
#endif
            sts = m_mfxSession.SetFrameAllocator(m_pGeneralAllocator);
        MSDK_CHECK_STATUS(sts, "m_mfxSession.SetFrameAllocator failed");
        m_bExternalAlloc = true;
    }

    // initialize memory allocator
    sts = m_pGeneralAllocator->Init(m_pmfxAllocatorParams);
    MSDK_CHECK_STATUS(sts, "m_pGeneralAllocator->Init failed");

    return MFX_ERR_NONE;
}

void CDecodingPipeline::DeleteFrames() {
    FreeBuffers();

    m_pCurrentFreeSurface = NULL;
    MSDK_SAFE_FREE(m_pCurrentFreeOutputSurface);

    m_pCurrentFreeVppSurface = NULL;

    // delete frames
    if (m_pGeneralAllocator) {
        m_pGeneralAllocator->Free(m_pGeneralAllocator->pthis, &m_mfxResponse);
    }

    return;
}

void CDecodingPipeline::DeleteAllocator() {
    // delete allocator
    MSDK_SAFE_DELETE(m_pGeneralAllocator);
    MSDK_SAFE_DELETE(m_pmfxAllocatorParams);
    MSDK_SAFE_DELETE(m_hwdev);
}
#if (MFX_VERSION < 2000)
void CDecodingPipeline::SetMultiView() {
    m_FileWriter.SetMultiView();
    m_bIsMVC = true;
}

mfxStatus CDecodingPipeline::AllocateExtMVCBuffers() {
    mfxU32 i;

    auto mvcBuffer = m_mfxVideoParams.GetExtBuffer<mfxExtMVCSeqDesc>();
    MSDK_CHECK_POINTER(mvcBuffer, MFX_ERR_MEMORY_ALLOC);

    mvcBuffer->View = new mfxMVCViewDependency[mvcBuffer->NumView];
    MSDK_CHECK_POINTER(mvcBuffer->View, MFX_ERR_MEMORY_ALLOC);
    for (i = 0; i < mvcBuffer->NumView; ++i) {
        MSDK_ZERO_MEMORY(mvcBuffer->View[i]);
    }
    mvcBuffer->NumViewAlloc = mvcBuffer->NumView;

    mvcBuffer->ViewId = new mfxU16[mvcBuffer->NumViewId];
    MSDK_CHECK_POINTER(mvcBuffer->ViewId, MFX_ERR_MEMORY_ALLOC);
    for (i = 0; i < mvcBuffer->NumViewId; ++i) {
        MSDK_ZERO_MEMORY(mvcBuffer->ViewId[i]);
    }
    mvcBuffer->NumViewIdAlloc = mvcBuffer->NumViewId;

    mvcBuffer->OP = new mfxMVCOperationPoint[mvcBuffer->NumOP];
    MSDK_CHECK_POINTER(mvcBuffer->OP, MFX_ERR_MEMORY_ALLOC);
    for (i = 0; i < mvcBuffer->NumOP; ++i) {
        MSDK_ZERO_MEMORY(mvcBuffer->OP[i]);
    }
    mvcBuffer->NumOPAlloc = mvcBuffer->NumOP;

    return MFX_ERR_NONE;
}

void CDecodingPipeline::DeallocateExtMVCBuffers() {
    auto mvcBuffer = m_mfxVideoParams.GetExtBuffer<mfxExtMVCSeqDesc>();
    if (mvcBuffer) {
        MSDK_SAFE_DELETE_ARRAY(mvcBuffer->View);
        MSDK_SAFE_DELETE_ARRAY(mvcBuffer->ViewId);
        MSDK_SAFE_DELETE_ARRAY(mvcBuffer->OP);
    }
}
#endif
mfxStatus CDecodingPipeline::ResetDecoder(sInputParams* pParams) {
    mfxStatus sts = MFX_ERR_NONE;

    // close decoder
    sts = m_pmfxDEC->Close();
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_NOT_INITIALIZED);
    MSDK_CHECK_STATUS(sts, "m_pmfxDEC->Close failed");

    // close VPP
    if (m_pmfxVPP) {
        sts = m_pmfxVPP->Close();
        MSDK_IGNORE_MFX_STS(sts, MFX_ERR_NOT_INITIALIZED);
        MSDK_CHECK_STATUS(sts, "m_pmfxVPP->Close failed");
    }

    // free allocated frames
    DeleteFrames();

    // initialize parameters with values from parsed header
    sts = InitMfxParams(pParams);
    MSDK_CHECK_STATUS(sts, "InitMfxParams failed");

    // in case of HW accelerated decode frames must be allocated prior to decoder initialization
    sts = AllocFrames();
    MSDK_CHECK_STATUS(sts, "AllocFrames failed");

    // init decoder
    sts = m_pmfxDEC->Init(&m_mfxVideoParams);
    if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
        msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
        MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    }
    MSDK_CHECK_STATUS(sts, "m_pmfxDEC->Init failed");

    if (m_pmfxVPP) {
        if (m_diMode)
            m_mfxVppVideoParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

        if (pParams->ScalingMode) {
            auto par         = m_mfxVppVideoParams.AddExtBuffer<mfxExtVPPScaling>();
            par->ScalingMode = pParams->ScalingMode;
        }

        sts = m_pmfxVPP->Init(&m_mfxVppVideoParams);
        if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
            msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
            MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
        }
        MSDK_CHECK_STATUS(sts, "m_pmfxVPP->Init failed");
    }

    return MFX_ERR_NONE;
}

mfxStatus CDecodingPipeline::DeliverOutput(mfxFrameSurface1* frame) {
    CAutoTimer timer_fwrite(m_tick_fwrite);

    mfxStatus res = MFX_ERR_NONE, sts = MFX_ERR_NONE;

    if (!frame) {
        return MFX_ERR_NULL_PTR;
    }

    if (m_bResetFileWriter) {
        sts = m_FileWriter.Reset();
        MSDK_CHECK_STATUS(sts, "");
        m_bResetFileWriter = false;
    }

#if (MFX_VERSION >= 2000)
    if (m_bExternalAlloc == true && m_bAPI2XInternalMem == false) {
#else
    if (m_bExternalAlloc) {
#endif
        if (m_eWorkMode == MODE_FILE_DUMP) {
            res = m_pGeneralAllocator->Lock(m_pGeneralAllocator->pthis,
                                            frame->Data.MemId,
                                            &(frame->Data));
            if (MFX_ERR_NONE == res) {
                res = m_bOutI420 ? m_FileWriter.WriteNextFrameI420(frame)
                                 : m_FileWriter.WriteNextFrame(frame);
                sts = m_pGeneralAllocator->Unlock(m_pGeneralAllocator->pthis,
                                                  frame->Data.MemId,
                                                  &(frame->Data));
            }
            if ((MFX_ERR_NONE == res) && (MFX_ERR_NONE != sts)) {
                res = sts;
            }
        }
        else if (m_eWorkMode == MODE_RENDERING) {
#if D3D_SURFACES_SUPPORT
            res = m_d3dRender.RenderFrame(frame, m_pGeneralAllocator);
#elif LIBVA_SUPPORT
            res = m_hwdev->RenderFrame(frame, m_pGeneralAllocator);
#endif

            msdk_tick current_tick = msdk_time_get_tick();
            while (m_delayTicks && (m_startTick + m_delayTicks > current_tick)) {
                msdk_tick left_tick = m_startTick + m_delayTicks - current_tick;
                uint32_t sleepTime  = (uint32_t)(left_tick * 1000 / msdk_time_get_frequency());
                MSDK_SLEEP(sleepTime);
                current_tick = msdk_time_get_tick();
            };
            m_startTick = msdk_time_get_tick();
        }
    }
    else {
        res = m_bOutI420 ? m_FileWriter.WriteNextFrameI420(frame)
                         : m_FileWriter.WriteNextFrame(frame);
    }

    return res;
}

void CDecodingPipeline::DeliverLoop(void) {
    while (!m_bStopDeliverLoop) {
        m_pDeliverOutputSemaphore->Wait();
        if (m_bStopDeliverLoop) {
            continue;
        }
        if (MFX_ERR_NONE != m_error) {
            continue;
        }
        msdkOutputSurface* pCurrentDeliveredSurface = m_DeliveredSurfacesPool.GetSurface();
        if (!pCurrentDeliveredSurface) {
            m_error = MFX_ERR_NULL_PTR;
            continue;
        }
        mfxFrameSurface1* frame = &(pCurrentDeliveredSurface->surface->frame);

        m_error = DeliverOutput(frame);
        ReturnSurfaceToBuffers(pCurrentDeliveredSurface);

        pCurrentDeliveredSurface = NULL;
        msdk_atomic_inc32(&m_output_count);
        m_pDeliveredEvent->Signal();
    }
    return;
}

void CDecodingPipeline::PrintPerFrameStat(bool force) {
#define MY_COUNT     1 // TODO: this will be cmd option
#define MY_THRESHOLD 10000.0
    if ((!(m_output_count % MY_COUNT) && (m_eWorkMode != MODE_PERFORMANCE)) || force) {
        double fps, fps_fread, fps_fwrite;

        m_timer_overall.Sync();

        fps = (m_tick_overall) ? m_output_count / CTimer::ConvertToSeconds(m_tick_overall) : 0.0;
        fps_fread = (m_tick_fread) ? m_output_count / CTimer::ConvertToSeconds(m_tick_fread) : 0.0;
        fps_fwrite =
            (m_tick_fwrite) ? m_output_count / CTimer::ConvertToSeconds(m_tick_fwrite) : 0.0;
        // decoding progress
        msdk_printf(
            MSDK_STRING("Frame number: %4d, fps: %0.3f, fread_fps: %0.3f, fwrite_fps: %.3f\r"),
            (int)m_output_count,
            fps,
            (fps_fread < MY_THRESHOLD) ? fps_fread : 0.0,
            (fps_fwrite < MY_THRESHOLD) ? fps_fwrite : 0.0);
        fflush(NULL);
#if D3D_SURFACES_SUPPORT
        m_d3dRender.UpdateTitle(fps);
#elif LIBVA_SUPPORT
        if (m_hwdev)
            m_hwdev->UpdateTitle(fps);
#endif
    }
}

mfxStatus CDecodingPipeline::SyncOutputSurface(mfxU32 wait) {
    if (!m_pCurrentOutputSurface) {
        m_pCurrentOutputSurface = m_OutputSurfacesPool.GetSurface();
    }
    if (!m_pCurrentOutputSurface) {
        return MFX_ERR_MORE_DATA;
    }

    mfxStatus sts = m_mfxSession.SyncOperation(m_pCurrentOutputSurface->syncp, wait);

    if (MFX_ERR_GPU_HANG == sts && m_bSoftRobustFlag) {
        msdk_printf(MSDK_STRING("GPU hang happened\n"));
        // Output surface can be corrupted
        // But should be delivered to output anyway
        sts = MFX_ERR_NONE;
    }

    if (MFX_WRN_IN_EXECUTION == sts) {
        return sts;
    }
    if (MFX_ERR_NONE == sts) {
        // we got completely decoded frame - pushing it to the delivering thread...
        ++m_synced_count;
        if (m_bPrintLatency) {
            m_vLatency.push_back(m_timer_overall.Sync() - m_pCurrentOutputSurface->surface->submit);
        }
        else {
            if (m_bAPI2XPerf == false)
                PrintPerFrameStat();
        }

#if (MFX_VERSION >= 2000)
        mfxFrameSurface1* mfxSurf = NULL;

        if (m_bAPI2XInternalMem == true || m_bAPI2XDecVPP == true) {
            mfxSurf = &m_pCurrentOutputSurface->surface->frame;
            sts     = mfxSurf->FrameInterface->Map(mfxSurf, MFX_MAP_READ);
            MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Map failed");
        }
#endif

        if (m_eWorkMode == MODE_PERFORMANCE) {
            m_output_count = m_synced_count;
            ReturnSurfaceToBuffers(m_pCurrentOutputSurface);
        }
        else if (m_eWorkMode == MODE_FILE_DUMP) {
            sts = DeliverOutput(&(m_pCurrentOutputSurface->surface->frame));
            if (MFX_ERR_NONE != sts) {
                sts = MFX_ERR_UNKNOWN;
            }
            else {
                m_output_count = m_synced_count;
            }
            ReturnSurfaceToBuffers(m_pCurrentOutputSurface);
        }
        else if (m_eWorkMode == MODE_RENDERING) {
            m_DeliveredSurfacesPool.AddSurface(m_pCurrentOutputSurface);
            m_pDeliveredEvent->Reset();
            m_pDeliverOutputSemaphore->Post();
        }

#if (MFX_VERSION >= 2000)
        if (mfxSurf && (m_bAPI2XInternalMem == true || m_bAPI2XDecVPP == true)) {
            sts = mfxSurf->FrameInterface->Unmap(mfxSurf);
            MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Unmap failed");

            sts = mfxSurf->FrameInterface->Release(mfxSurf);
            MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Release failed");
        }
#endif
        m_pCurrentOutputSurface = NULL;
    }

    return sts;
}

mfxStatus CDecodingPipeline::RunDecoding() {
    mfxFrameSurface1* pDecOutSurface = NULL;
#if (MFX_VERSION >= 2000)
    mfxFrameSurface1* pVPPOutSurface = NULL;
#endif
    mfxBitstream* pBitstream         = &m_mfxBS;
    mfxStatus sts                    = MFX_ERR_NONE;
    bool bErrIncompatibleVideoParams = false;
    CTimeInterval<> decodeTimer(m_bIsCompleteFrame);
    time_t start_time = time(0);
    std::thread deliverThread;

    if (m_eWorkMode == MODE_RENDERING) {
        m_pDeliverOutputSemaphore = new MSDKSemaphore(sts);
        m_pDeliveredEvent         = new MSDKEvent(sts, false, false);

        deliverThread = std::thread(&CDecodingPipeline::DeliverLoop, this);
    }

#if (MFX_VERSION >= 2000)
    auto api2x_perf_t1 = std::chrono::high_resolution_clock::now();
#endif

    while (((sts == MFX_ERR_NONE) || (MFX_ERR_MORE_DATA == sts) || (MFX_ERR_MORE_SURFACE == sts)) &&
           (m_nFrames > m_output_count)) {
        if (MFX_ERR_NONE != m_error) {
            msdk_printf(MSDK_STRING("DeliverOutput return error = %d\n"), (int)m_error);
            break;
        }

        if (pBitstream &&
            ((MFX_ERR_MORE_DATA == sts) || (m_bIsCompleteFrame && !pBitstream->DataLength))) {
            CAutoTimer timer_fread(m_tick_fread);
            sts = m_FileReader->ReadNextFrame(pBitstream); // read more data to input bit stream

            if (MFX_ERR_MORE_DATA == sts) {
                sts = MFX_ERR_NONE;
                // Timeout has expired or videowall mode
                m_timer_overall.Sync();
                if (((CTimer::ConvertToSeconds(m_tick_overall) < m_nTimeout) && m_nTimeout) ||
                    m_bIsVideoWall) {
                    m_FileReader->Reset();
                    m_bResetFileWriter = true;

                    // Reset bitstream state
                    pBitstream->DataFlag = 0;

                    continue;
                }

                // we almost reached end of stream, need to pull buffered data now
                pBitstream = NULL;
            }
        }

        if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) || (MFX_ERR_MORE_SURFACE == sts)) {
            // here we check whether output is ready, though we do not wait...
#ifndef __SYNC_WA
            mfxStatus _sts = SyncOutputSurface(0);
            if (MFX_ERR_UNKNOWN == _sts) {
                sts = _sts;
                break;
            }
            else if (MFX_ERR_NONE == _sts) {
                continue;
            }
#endif
        }
        else {
            MSDK_CHECK_STATUS_NO_RET(sts, "ReadNextFrame failed");
        }

        if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) || (MFX_ERR_MORE_SURFACE == sts)) {
#if (MFX_VERSION >= 2000)
            if (m_bAPI2XInternalMem == false && m_bAPI2XDecVPP == false)
#endif
            {
                SyncFrameSurfaces();
                if (!m_pCurrentFreeSurface) {
                    m_pCurrentFreeSurface = m_FreeSurfacesPool.GetSurface();
                }

                SyncVppFrameSurfaces();
                if (!m_pCurrentFreeVppSurface) {
                    m_pCurrentFreeVppSurface = m_FreeVppSurfacesPool.GetSurface();
                }
            }

#if (MFX_VERSION >= 2000)
            if ((!m_pCurrentFreeSurface &&
                 (m_bAPI2XInternalMem == false && m_bAPI2XDecVPP == false))
    #ifndef __SYNC_WA
                || !m_pCurrentFreeVppSurface &&
                       (m_bAPI2XInternalMem == false && m_bAPI2XDecVPP == false)) {
    #else
                || ((!m_pCurrentFreeVppSurface && m_bVppIsUsed) &&
                    (m_bAPI2XInternalMem == false && m_bAPI2XDecVPP == false)) ||
                (m_OutputSurfacesPool.GetSurfaceCount() == m_mfxVideoParams.AsyncDepth)) {
    #endif
#else
    #ifndef __SYNC_WA
                    || !m_pCurrentFreeVppSurface) {
    #else
                || (!m_pCurrentFreeVppSurface && m_bVppIsUsed) ||
                    (m_OutputSurfacesPool.GetSurfaceCount() == m_mfxVideoParams.AsyncDepth)) {
    #endif
#endif
                // we stuck with no free surface available, now we will sync...
                sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
                if (MFX_ERR_MORE_DATA == sts) {
                    if ((m_eWorkMode == MODE_PERFORMANCE) || (m_eWorkMode == MODE_FILE_DUMP)) {
                        sts = MFX_ERR_NOT_FOUND;
                    }
                    else if (m_eWorkMode == MODE_RENDERING) {
                        if (m_synced_count != m_output_count) {
                            sts = m_pDeliveredEvent->TimedWait(MSDK_DEC_WAIT_INTERVAL);
                        }
                        else {
                            sts = MFX_ERR_NOT_FOUND;
                        }
                    }
                    if (MFX_ERR_NOT_FOUND == sts) {
                        msdk_printf(
                            MSDK_STRING("fatal: failed to find output surface, that's a bug!\n"));
                        break;
                    }
                }
                // note: MFX_WRN_IN_EXECUTION will also be treated as an error at this point
                continue;
            }

            if (!m_pCurrentFreeOutputSurface) {
                m_pCurrentFreeOutputSurface = GetFreeOutputSurface();
            }
            if (!m_pCurrentFreeOutputSurface) {
                sts = MFX_ERR_NOT_FOUND;
                break;
            }
        }

        // exit by timeout
        if ((MFX_ERR_NONE == sts) && m_bIsVideoWall && (time(0) - start_time) >= m_nTimeout) {
            sts = MFX_ERR_NONE;
            break;
        }

        // use fused decvpp function for decode + vpp pipeline, when api2 decvpp flag is set
        if (m_bAPI2XDecVPP == true) {
            sts = m_pmfxDecVPP->DecodeFrameAsync(pBitstream, NULL, 0, &m_pDecVPPOutSurfaces);
            if (pBitstream && MFX_ERR_MORE_DATA == sts &&
                pBitstream->MaxLength == pBitstream->DataLength) {
                m_mfxBS.Extend(pBitstream->MaxLength * 2);
            }

            if (MFX_ERR_NONE == sts) {
                // we are not going to save the decode output, but just release it for now
                sts = m_pDecVPPOutSurfaces->Surfaces[0]->FrameInterface->Release(
                    m_pDecVPPOutSurfaces->Surfaces[0]);
                MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Release failed");

                // vpp out
                if (m_pCurrentFreeOutputSurface) {
                    msdkFrameSurface* surface = FindUsedSurface(m_pDecVPPOutSurfaces->Surfaces[1]);
                    msdk_atomic_inc16(&(surface->render_lock));
                    m_pCurrentFreeOutputSurface->surface = surface;
                    m_OutputSurfacesPool.AddSurface(m_pCurrentFreeOutputSurface);
                    m_pCurrentFreeOutputSurface = NULL;
                }
                else {
                    sts = MFX_ERR_MEMORY_ALLOC;
                    msdk_printf(MSDK_STRING("fatal: failed to add vpp output surface\n"));
                }
            }
            else if ((MFX_ERR_MORE_DATA == sts) && !pBitstream) {
                // that's it - we reached end of stream; now we need to render bufferred data...
                do {
                    sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
                } while (MFX_ERR_NONE == sts);

                MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
                if (sts)
                    MSDK_PRINT_WRN_MSG(sts, "SyncOutputSurface failed")

                while (m_synced_count != m_output_count) {
                    m_pDeliveredEvent->Wait();
                }
                break;
            }
        }
        else {
            if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) ||
                (MFX_ERR_MORE_SURFACE == sts)) {
                if (m_bIsCompleteFrame) {
#if (MFX_VERSION >= 2000)
                    if (m_bAPI2XInternalMem == false && m_bAPI2XDecVPP == false)
#endif
                        if (m_pCurrentFreeSurface)
                            m_pCurrentFreeSurface->submit = m_timer_overall.Sync();
                }
                pDecOutSurface = NULL;
                do {
#if (MFX_VERSION >= 1025)
                    mfxExtDecodeErrorReport* errorReport = nullptr;
                    if (pBitstream) {
                        errorReport =
                            (mfxExtDecodeErrorReport*)GetExtBuffer(pBitstream->ExtParam,
                                                                   pBitstream->NumExtParam,
                                                                   MFX_EXTBUFF_DECODE_ERROR_REPORT);
                    }
#endif
                    sts = m_pmfxDEC->DecodeFrameAsync(
                        pBitstream,
#if (MFX_VERSION >= 2000)
                        (m_bAPI2XInternalMem == true) ? nullptr : &(m_pCurrentFreeSurface->frame),
#else
                        &(m_pCurrentFreeSurface->frame),
#endif
                        &pDecOutSurface,
                        &(m_pCurrentFreeOutputSurface->syncp));

#if (MFX_VERSION >= 1025)
                    PrintDecodeErrorReport(errorReport);
#endif

                    if (pBitstream && MFX_ERR_MORE_DATA == sts &&
                        pBitstream->MaxLength == pBitstream->DataLength) {
                        m_mfxBS.Extend(pBitstream->MaxLength * 2);
                    }

                    if (MFX_WRN_DEVICE_BUSY == sts) {
                        if (m_bIsCompleteFrame) {
                            //in low latency mode device busy leads to increasing of latency
                            //msdk_printf(MSDK_STRING("Warning : latency increased due to MFX_WRN_DEVICE_BUSY\n"));
                        }
                        mfxStatus _sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
                        // note: everything except MFX_ERR_NONE are errors at this point
                        if (MFX_ERR_NONE == _sts) {
                            sts = MFX_WRN_DEVICE_BUSY;
                        }
                        else {
                            sts = _sts;
                            if (MFX_ERR_MORE_DATA == sts) {
                                // we can't receive MFX_ERR_MORE_DATA and have no output - that's a bug
                                sts = MFX_WRN_DEVICE_BUSY; //MFX_ERR_NOT_FOUND;
                            }
                        }
                    }
                } while (MFX_WRN_DEVICE_BUSY == sts);

                if (sts > MFX_ERR_NONE) {
                    // ignoring warnings...
                    if (m_pCurrentFreeOutputSurface->syncp) {
                        MSDK_SELF_CHECK(pDecOutSurface);
                        // output is available
                        sts = MFX_ERR_NONE;
                    }
                    else {
                        // output is not available
                        sts = MFX_ERR_MORE_SURFACE;
                    }
                }
                else if ((MFX_ERR_MORE_DATA == sts) && pBitstream) {
                    if (m_bIsCompleteFrame && pBitstream->DataLength) {
                        // In low_latency mode decoder have to process bitstream completely
                        msdk_printf(MSDK_STRING(
                            "error: Incorrect decoder behavior in low latency mode (bitstream length is not equal to 0 after decoding)\n"));
                        sts = MFX_ERR_UNDEFINED_BEHAVIOR;
                        continue;
                    }
                }
                else if ((MFX_ERR_MORE_DATA == sts) && !pBitstream) {
                    // that's it - we reached end of stream; now we need to render bufferred data...
                    do {
                        sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
                    } while (MFX_ERR_NONE == sts);

                    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
                    if (sts)
                        MSDK_PRINT_WRN_MSG(sts, "SyncOutputSurface failed")

                    while (m_synced_count != m_output_count) {
                        m_pDeliveredEvent->Wait();
                    }
                    break;
                }
                else if (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM == sts) {
                    bErrIncompatibleVideoParams = true;
                    // need to go to the buffering loop prior to reset procedure
                    pBitstream = NULL;
                    sts        = MFX_ERR_NONE;
                    continue;
                }
                else if (MFX_ERR_REALLOC_SURFACE == sts) {
                    mfxVideoParam param{};
                    sts = m_pmfxDEC->GetVideoParam(&param);
                    if (MFX_ERR_NONE != sts) {
                        // need to go to the buffering loop prior to reset procedure
                        pBitstream = NULL;
                        sts        = MFX_ERR_NONE;
                        continue;
                    }

                    sts = ReallocCurrentSurface(param.mfx.FrameInfo);
                    if (MFX_ERR_NONE != sts) {
                        // need to go to the buffering loop prior to reset procedure
                        pBitstream = NULL;
                        sts        = MFX_ERR_NONE;
                    }
                    continue;
                }
            }

            if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) ||
                (MFX_ERR_MORE_SURFACE == sts)) {
                // if current free surface is locked we are moving it to the used surfaces array
                /*if (m_pCurrentFreeSurface->frame.Data.Locked)*/
#if (MFX_VERSION >= 2000)
                if (m_bAPI2XInternalMem == false)
#endif
                {
                    if (m_pCurrentFreeSurface) {
                        m_UsedSurfacesPool.AddSurface(m_pCurrentFreeSurface);
                        m_pCurrentFreeSurface = NULL;
                    }
                }
            }
            else {
                MSDK_CHECK_STATUS_NO_RET(sts, "DecodeFrameAsync returned error status");
            }

            if (MFX_ERR_NONE == sts) {
                if (m_bVppIsUsed) {
#if (MFX_VERSION >= 2000)
                    if (m_bAPI2XInternalMem == true) {
                        pVPPOutSurface = NULL;

                        sts = m_pmfxMemory->GetSurfaceForVPPOut(&pVPPOutSurface);
                        MSDK_CHECK_STATUS(sts, "Unknown error in MFXMemory_GetSurfaceForVPPOut");

                        // WA: RunFrameVPPAsync doesn't copy ViewId from input to output
                        pVPPOutSurface->Info.FrameId.ViewId = pDecOutSurface->Info.FrameId.ViewId;

                        sts = pDecOutSurface->FrameInterface->Map(pDecOutSurface, MFX_MAP_READ);
                        MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Map failed");

                        sts = m_pmfxVPP->RunFrameVPPAsync(pDecOutSurface,
                                                          pVPPOutSurface,
                                                          NULL,
                                                          &(m_pCurrentFreeOutputSurface->syncp));

                        // process errors
                        if (MFX_ERR_MORE_DATA == sts) { // will never happen actually
                            continue;
                        }
                        else if (MFX_ERR_NONE != sts) {
                            MSDK_PRINT_RET_MSG(sts, "RunFrameVPPAsync failed");
                            break;
                        }

                        sts = pDecOutSurface->FrameInterface->Unmap(pDecOutSurface);
                        MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Unmap failed");
                        sts = pDecOutSurface->FrameInterface->Release(pDecOutSurface);
                        MSDK_CHECK_STATUS(sts, "mfxFrameSurfaceInterface->Release failed");

                        msdkFrameSurface* surface = FindUsedSurface(pVPPOutSurface);
                        msdk_atomic_inc16(&(surface->render_lock));

                        m_pCurrentFreeOutputSurface->surface = surface;
                        m_OutputSurfacesPool.AddSurface(m_pCurrentFreeOutputSurface);
                        m_pCurrentFreeOutputSurface = NULL;
                    }
                    else // if m_bAPI2XInternalMem is false, execute legacy path
#endif
                        if (m_pCurrentFreeVppSurface) {
                        do {
                            if ((m_pCurrentFreeVppSurface->frame.Info.CropW == 0) ||
                                (m_pCurrentFreeVppSurface->frame.Info.CropH == 0)) {
                                m_pCurrentFreeVppSurface->frame.Info.CropW =
                                    pDecOutSurface->Info.CropW;
                                m_pCurrentFreeVppSurface->frame.Info.CropH =
                                    pDecOutSurface->Info.CropH;
                                m_pCurrentFreeVppSurface->frame.Info.CropX =
                                    pDecOutSurface->Info.CropX;
                                m_pCurrentFreeVppSurface->frame.Info.CropY =
                                    pDecOutSurface->Info.CropY;
                            }
                            if (pDecOutSurface->Info.PicStruct !=
                                m_pCurrentFreeVppSurface->frame.Info.PicStruct) {
                                m_pCurrentFreeVppSurface->frame.Info.PicStruct =
                                    pDecOutSurface->Info.PicStruct;
                            }
                            if ((pDecOutSurface->Info.PicStruct == 0) &&
                                (m_pCurrentFreeVppSurface->frame.Info.PicStruct == 0)) {
                                m_pCurrentFreeVppSurface->frame.Info.PicStruct =
                                    pDecOutSurface->Info.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
                            }

                            if (m_diMode)
                                m_pCurrentFreeVppSurface->frame.Info.PicStruct =
                                    MFX_PICSTRUCT_PROGRESSIVE;

                            // WA: RunFrameVPPAsync doesn't copy ViewId from input to output
                            m_pCurrentFreeVppSurface->frame.Info.FrameId.ViewId =
                                pDecOutSurface->Info.FrameId.ViewId;
                            sts =
                                m_pmfxVPP->RunFrameVPPAsync(pDecOutSurface,
                                                            &(m_pCurrentFreeVppSurface->frame),
                                                            NULL,
                                                            &(m_pCurrentFreeOutputSurface->syncp));

                            if (MFX_WRN_DEVICE_BUSY == sts) {
                                MSDK_SLEEP(
                                    1); // just wait and then repeat the same call to RunFrameVPPAsync
                            }
                        } while (MFX_WRN_DEVICE_BUSY == sts);

                        // process errors
                        if (MFX_ERR_MORE_DATA == sts) { // will never happen actually
                            continue;
                        }
                        else if (MFX_ERR_NONE != sts) {
                            MSDK_PRINT_RET_MSG(sts, "RunFrameVPPAsync failed");
                            break;
                        }

                        m_UsedVppSurfacesPool.AddSurface(m_pCurrentFreeVppSurface);
                        msdk_atomic_inc16(&(m_pCurrentFreeVppSurface->render_lock));

                        m_pCurrentFreeOutputSurface->surface = m_pCurrentFreeVppSurface;
                        m_OutputSurfacesPool.AddSurface(m_pCurrentFreeOutputSurface);

                        m_pCurrentFreeOutputSurface = NULL;
                        m_pCurrentFreeVppSurface    = NULL;
                    }
                }
                else {
                    msdkFrameSurface* surface = FindUsedSurface(pDecOutSurface);

                    msdk_atomic_inc16(&(surface->render_lock));

                    m_pCurrentFreeOutputSurface->surface = surface;
                    m_OutputSurfacesPool.AddSurface(m_pCurrentFreeOutputSurface);
                    m_pCurrentFreeOutputSurface = NULL;
                }
            }
        }
    } //while processing

#if (MFX_VERSION >= 2000)
    auto api2x_perf_t2  = std::chrono::high_resolution_clock::now();
    m_api2xPerfLoopTime = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(api2x_perf_t2 - api2x_perf_t1)
            .count());
#endif

    if (m_nFrames == m_output_count) {
        if (sts != MFX_ERR_NONE)
            msdk_printf(
                MSDK_STRING(
                    "[WARNING] Decoder returned error %s that could be compensated during next iterations of decoding process.\
                                    But requested amount of frames is already successfully decoded, so whole process is finished successfully."),
                StatusToString(sts).c_str());
        sts = MFX_ERR_NONE;
    }

    if (m_bAPI2XPerf == false)
        PrintPerFrameStat(true);

    if (m_bPrintLatency && m_vLatency.size() > 0) {
        unsigned int frame_idx = 0;
        msdk_tick sum          = 0;
        for (std::vector<msdk_tick>::iterator it = m_vLatency.begin(); it != m_vLatency.end();
             ++it) {
            sum += *it;
            msdk_printf(MSDK_STRING("Frame %4d, latency=%5.5f ms\n"),
                        ++frame_idx,
                        (double)(CTimer::ConvertToSeconds(*it) * 1000));
        }
        msdk_printf(MSDK_STRING("\nLatency summary:\n"));
        msdk_printf(
            MSDK_STRING("\nAVG=%5.5f ms, MAX=%5.5f ms, MIN=%5.5f ms"),
            (double)(CTimer::ConvertToSeconds((msdk_tick)((mfxF64)sum / m_vLatency.size())) * 1000),
            (double)(CTimer::ConvertToSeconds(
                         *std::max_element(m_vLatency.begin(), m_vLatency.end())) *
                     1000),
            (double)(CTimer::ConvertToSeconds(
                         *std::min_element(m_vLatency.begin(), m_vLatency.end())) *
                     1000));
    }

    if (m_eWorkMode == MODE_RENDERING) {
        m_bStopDeliverLoop = true;
        m_pDeliverOutputSemaphore->Post();

        if (deliverThread.joinable())
            deliverThread.join();
    }

    MSDK_SAFE_DELETE(m_pDeliverOutputSemaphore);
    MSDK_SAFE_DELETE(m_pDeliveredEvent);

    // exit in case of other errors
    MSDK_CHECK_STATUS(sts, "Unexpected error!!");

    // if we exited main decoding loop with ERR_INCOMPATIBLE_PARAM we need to send this status to caller
    if (bErrIncompatibleVideoParams) {
        sts = MFX_ERR_INCOMPATIBLE_VIDEO_PARAM;
    }

    return sts; // ERR_NONE or ERR_INCOMPATIBLE_VIDEO_PARAM
}

void CDecodingPipeline::PrintInfo() {
    msdk_printf(MSDK_STRING("Decoding Sample Version %s\n\n"), GetMSDKSampleVersion().c_str());
    msdk_printf(MSDK_STRING("\nInput video\t%s\n"),
                CodecIdToStr(m_mfxVideoParams.mfx.CodecId).c_str());
    if (m_bVppIsUsed) {
        msdk_printf(MSDK_STRING("Output format\t%s (using vpp)\n"),
                    m_bOutI420 ? MSDK_STRING("I420(YUV)")
                               : CodecIdToStr(m_mfxVppVideoParams.vpp.Out.FourCC).c_str());
    }
    else {
        msdk_printf(MSDK_STRING("Output format\t%s\n"),
                    m_bOutI420 ? MSDK_STRING("I420(YUV)")
                               : CodecIdToStr(m_mfxVideoParams.mfx.FrameInfo.FourCC).c_str());
    }

    mfxFrameInfo Info = m_mfxVideoParams.mfx.FrameInfo;
    msdk_printf(MSDK_STRING("Input:\n"));
    msdk_printf(MSDK_STRING("  Resolution\t%dx%d\n"), Info.Width, Info.Height);
    msdk_printf(MSDK_STRING("  Crop X,Y,W,H\t%d,%d,%d,%d\n"),
                Info.CropX,
                Info.CropY,
                Info.CropW,
                Info.CropH);
    msdk_printf(MSDK_STRING("Output:\n"));
    if (m_vppOutHeight && m_vppOutWidth) {
        msdk_printf(MSDK_STRING("  Resolution\t%dx%d\n"), (int)m_vppOutWidth, (int)m_vppOutHeight);
    }
    else {
        msdk_printf(MSDK_STRING("  Resolution\t%dx%d\n"),
                    Info.CropW ? Info.CropW : Info.Width,
                    Info.CropH ? Info.CropH : Info.Height);
    }

    mfxF64 dFrameRate = CalculateFrameRate(Info.FrameRateExtN, Info.FrameRateExtD);
    msdk_printf(MSDK_STRING("Frame rate\t%.2f\n"), (double)dFrameRate);

    const msdk_char* sMemType =
#if defined(_WIN32) || defined(_WIN64)
        m_memType == D3D9_MEMORY
            ? MSDK_STRING("d3d")
#else
        m_memType == D3D9_MEMORY
            ? MSDK_STRING("vaapi")
#endif
            : (m_memType == D3D11_MEMORY ? MSDK_STRING("d3d11") : MSDK_STRING("system"));
    msdk_printf(MSDK_STRING("Memory type\t\t%s\n"), sMemType);

    const msdk_char* sImpl = (MFX_IMPL_VIA_D3D11 == MFX_IMPL_VIA_MASK(m_impl))
                                 ? MSDK_STRING("hw_d3d11")
                                 : (MFX_IMPL_SOFTWARE == MFX_IMPL_BASETYPE(m_impl))
                                       ? MSDK_STRING("sw")
                                       : MSDK_STRING("hw");
    msdk_printf(MSDK_STRING("MediaSDK impl\t\t%s\n"), sImpl);

    mfxVersion ver;
    m_mfxSession.QueryVersion(&ver);
    msdk_printf(MSDK_STRING("MediaSDK version\t%d.%d\n"), ver.Major, ver.Minor);

    msdk_printf(MSDK_STRING("\n"));

    return;
}
