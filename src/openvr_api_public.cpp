//========= Copyright Valve Corporation ============//
#define VR_API_EXPORT 1
#include "openvr.h"
#include "openvr_capi.h"
#include "ivrclientcore.h"
#include <vrcommon/pathtools_public.h>
#include <vrcommon/sharedlibtools_public.h>
#include <vrcommon/envvartools_public.h>
#include "hmderrors_public.h"
#include <vrcommon/strtools_public.h>
#include <vrcommon/vrpathregistry_public.h>
#include <mutex>

#include "VrHooks.h"
#include "Config.h"
#undef interface

//using vr::EVRInitError;
using vr::IVRSystem;
using vr::IVRClientCore;
using vr::VRInitError_None;

// figure out how to import from the VR API dll
#if defined(_WIN32)

#if !defined(OPENVR_BUILD_STATIC)
#define VR_EXPORT_INTERFACE extern "C" __declspec( dllexport )
#else
#define VR_EXPORT_INTERFACE extern "C"
#endif

#elif defined(__GNUC__) || defined(COMPILER_GCC) || defined(__APPLE__)

#define VR_EXPORT_INTERFACE extern "C" __attribute__((visibility("default")))


#else
#error "Unsupported Platform."
#endif

namespace vr
{
namespace {
}

static void *g_pVRModule = NULL;
static IVRClientCore *g_pHmdSystem = NULL;
static std::recursive_mutex g_mutexSystem;

typedef void* (*VRClientCoreFactoryFn)(const char *pInterfaceName, int *pReturnCode);

static uint32_t g_nVRToken = 0;

uint32_t VR_GetInitToken()
{
	return g_nVRToken;
}

EVRInitError VR_LoadHmdSystemInternal();
void CleanupInternalInterfaces();


uint32_t VR_InitInternal2( EVRInitError *peError, vr::EVRApplicationType eApplicationType, const char *pStartupInfo )
{
	Log() << "VR_InitInternal2\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	InitHooks();
	
	EVRInitError err = VR_LoadHmdSystemInternal();
	if ( err == vr::VRInitError_None )
	{
		Log() << "VR_InitInternal2-1\n";
		err = g_pHmdSystem->Init( eApplicationType, pStartupInfo );
	}

	if ( peError )
		*peError = err;

	if ( err != VRInitError_None )
	{
		Log() << "VR_InitInternal2-2\n";
		SharedLib_Unload( g_pVRModule );
		g_pHmdSystem = NULL;
		g_pVRModule = NULL;

		return 0;
	}

	Log() << "VR_InitInternal2-3\n";
	return ++g_nVRToken;
}

VR_INTERFACE uint32_t VR_CALLTYPE VR_InitInternal( EVRInitError *peError, EVRApplicationType eApplicationType );

uint32_t VR_InitInternal( EVRInitError *peError, vr::EVRApplicationType eApplicationType )
{
	return VR_InitInternal2( peError, eApplicationType, nullptr );
}

void VR_ShutdownInternal()
{
	Log() << "VR_ShutdownInternal\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	ShutdownHooks();
	
#if !defined( VR_API_PUBLIC )
	CleanupInternalInterfaces();
#endif

	if ( g_pHmdSystem )
	{
		g_pHmdSystem->Cleanup();
		g_pHmdSystem = NULL;
	}

	if ( g_pVRModule )
	{
		SharedLib_Unload( g_pVRModule );
		g_pVRModule = NULL;
	}

	++g_nVRToken;
}

EVRInitError VR_LoadHmdSystemInternal()
{
	Log() << "VR_LoadHmdSystemInternal\n";
	std::string sRuntimePath, sConfigPath, sLogPath;

	bool bReadPathRegistry = CVRPathRegistry_Public::GetPaths( &sRuntimePath, &sConfigPath, &sLogPath, NULL, NULL );
	if( !bReadPathRegistry )
	{
		return vr::VRInitError_Init_PathRegistryNotFound;
	}

	// figure out where we're going to look for vrclient.dll
	// see if the specified path actually exists.
	if( !Path_IsDirectory( sRuntimePath ) )
	{
		return vr::VRInitError_Init_InstallationNotFound;
	}

	// Because we don't have a way to select debug vs. release yet we'll just
	// use debug if it's there
#if defined( LINUX64 ) || defined( LINUXARM64 )
	std::string sTestPath = Path_Join( sRuntimePath, "bin", PLATSUBDIR );
#else
	std::string sTestPath = Path_Join( sRuntimePath, "bin" );
#endif
	if( !Path_IsDirectory( sTestPath ) )
	{
		return vr::VRInitError_Init_InstallationCorrupt;
	}

#if defined( WIN64 )
	std::string sDLLPath = Path_Join( sTestPath, "vrclient_x64" DYNAMIC_LIB_EXT );
#else
	std::string sDLLPath = Path_Join( sTestPath, "vrclient" DYNAMIC_LIB_EXT );
#endif

	// only look in the override
	void *pMod = SharedLib_Load( sDLLPath.c_str() );
	// nothing more to do if we can't load the DLL
	if( !pMod )
	{
		return vr::VRInitError_Init_VRClientDLLNotFound;
	}

	VRClientCoreFactoryFn fnFactory = ( VRClientCoreFactoryFn )( SharedLib_GetFunction( pMod, "VRClientCoreFactory" ) );
	if( !fnFactory )
	{
		SharedLib_Unload( pMod );
		return vr::VRInitError_Init_FactoryNotFound;
	}

	int nReturnCode = 0;
	g_pHmdSystem = static_cast< IVRClientCore * > ( fnFactory( vr::IVRClientCore_Version, &nReturnCode ) );
	if( !g_pHmdSystem )
	{
		SharedLib_Unload( pMod );
		return vr::VRInitError_Init_InterfaceNotFound;
	}

	g_pVRModule = pMod;
	Log() << "Initialised\n";
	return VRInitError_None;
}

void *VR_GetGenericInterface(const char *pchInterfaceVersion, EVRInitError *peError)
{
	Log() << "VR_GetGenericInterface\n";
	//Log() << "Requesting interface: " << pchInterfaceVersion;
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	if (!g_pHmdSystem)
	{
		if (peError)
			*peError = vr::VRInitError_Init_NotInitialized;
		return NULL;
	}

	// if C interfaces were requested, make sure that we also request the underlying
	// C++ interfaces so that our hooks get installed.
	std::string interfaceName (pchInterfaceVersion);
	if (interfaceName.substr(0, 7) == "FnTable") {
		// C interfaces have names "FnTable:IVRxxx", so strip the "FnTable:"
		VR_GetGenericInterface(interfaceName.substr(8).c_str(), nullptr);
	}

	void *interface = g_pHmdSystem->GetGenericInterface(pchInterfaceVersion, peError);
	//Log() << " got: " << interface << "\n";
	HookVRInterface(pchInterfaceVersion, interface);

	return interface;
}

bool VR_IsInterfaceVersionValid(const char *pchInterfaceVersion)
{
	Log() << "VR_IsInterfaceVersionValid\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	if (!g_pHmdSystem)
	{
		return false;
	}

	return g_pHmdSystem->IsInterfaceVersionValid(pchInterfaceVersion) == VRInitError_None;
}

bool VR_IsHmdPresent()
{
	Log() << "VR_IsHmdPresent\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	if( g_pHmdSystem )
	{
		// if we're already initialized, just call through
		return g_pHmdSystem->BIsHmdPresent();
	}
	else
	{
		// otherwise we need to do a bit more work
		EVRInitError err = VR_LoadHmdSystemInternal();
		if( err != VRInitError_None )
			return false;

		bool bHasHmd = g_pHmdSystem->BIsHmdPresent();

		g_pHmdSystem = NULL;
		SharedLib_Unload( g_pVRModule );
		g_pVRModule = NULL;

		return bHasHmd;
	}
}

/** Returns true if the OpenVR runtime is installed. */
bool VR_IsRuntimeInstalled()
{
	Log() << "VR_IsRuntimeInstalled\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	if( g_pHmdSystem )
	{
		// if we're already initialized, OpenVR is obviously installed
		return true;
	}
	else
	{
		// otherwise we need to do a bit more work
		std::string sRuntimePath, sConfigPath, sLogPath;

		bool bReadPathRegistry = CVRPathRegistry_Public::GetPaths( &sRuntimePath, &sConfigPath, &sLogPath, NULL, NULL );
		if( !bReadPathRegistry )
		{
			return false;
		}

		// figure out where we're going to look for vrclient.dll
		// see if the specified path actually exists.
		if( !Path_IsDirectory( sRuntimePath ) )
		{
			return false;
		}

		// the installation may be corrupt in some way, but it certainly looks installed
		return true;
	}
}


// -------------------------------------------------------------------------------
// Purpose: This is the old Runtime Path interface that is no longer exported in the
//			latest header. We still want to export it from the DLL, though, so updating
//			to a new DLL doesn't break old compiled code. This version was not thread 
//			safe and could change the buffer pointer to by a previous result on a 
//			subsequent call
// -------------------------------------------------------------------------------
//VR_EXPORT_INTERFACE const char *VR_CALLTYPE VR_RuntimePath();
//
///** Returns where OpenVR runtime is installed. */
//const char *VR_RuntimePath()
//{
//	Log() << "VR_RuntimePath\n";
//	static char rchBuffer[1024];
//	uint32_t unRequiredSize;
//	if ( VR_GetRuntimePath( rchBuffer, sizeof( rchBuffer ), &unRequiredSize ) && unRequiredSize < sizeof( rchBuffer ) )
//	{
//		return rchBuffer;
//	}
//	else
//	{
//		return nullptr;
//	}
//}


/** Returns where OpenVR runtime is installed. */
bool VR_GetRuntimePath( char *pchPathBuffer, uint32_t unBufferSize, uint32_t *punRequiredBufferSize )
{
	Log() << "VR_GetRuntimePath\n";
	// otherwise we need to do a bit more work
	std::string sRuntimePath;

	*punRequiredBufferSize = 0;

	bool bReadPathRegistry = CVRPathRegistry_Public::GetPaths( &sRuntimePath, nullptr, nullptr, nullptr, nullptr );
	if ( !bReadPathRegistry )
	{
		return false;
	}

	// figure out where we're going to look for vrclient.dll
	// see if the specified path actually exists.
	if ( !Path_IsDirectory( sRuntimePath ) )
	{
		return false;
	}

	*punRequiredBufferSize = (uint32_t)sRuntimePath.size() + 1;
	if ( sRuntimePath.size() >= unBufferSize )
	{
		*pchPathBuffer = '\0';
	}
	else
	{
		strcpy_safe( pchPathBuffer, unBufferSize, sRuntimePath.c_str() );
	}

	return true;
}


/** Returns the symbol version of an HMD error. */
const char *VR_GetVRInitErrorAsSymbol( EVRInitError error )
{
	Log() << "VR_GetVRInitErrorAsSymbol\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	if( g_pHmdSystem )
		return g_pHmdSystem->GetIDForVRInitError( error );
	else
		return GetIDForVRInitError( error );
}


/** Returns the english string version of an HMD error. */
const char *VR_GetVRInitErrorAsEnglishDescription( EVRInitError error )
{
	Log() << "VR_GetVRInitErrorAsEnglishDescription\n";
	std::lock_guard<std::recursive_mutex> lock( g_mutexSystem );

	if ( g_pHmdSystem )
		return g_pHmdSystem->GetEnglishStringForHmdError( error );
	else
		return GetEnglishStringForHmdError( error );
}


VR_INTERFACE const char *VR_CALLTYPE VR_GetStringForHmdError( vr::EVRInitError error );

/** Returns the english string version of an HMD error. */
const char *VR_GetStringForHmdError( EVRInitError error )
{
	Log() << "VR_GetStringForHmdError\n";
	return VR_GetVRInitErrorAsEnglishDescription( error );
}

//VR_INTERFACE IVRSystem* VR_Init(EVRInitError* peError, EVRApplicationType eApplicationType);

inline uint32_t& VRToken()
{
	static uint32_t token;
	return token;
}

inline void outputError(const char* function, const EVRInitError& eError)
{
	//if (eError != VRInitError_None)
	{
		Log() << function << " " << VR_GetVRInitErrorAsEnglishDescription(eError);
	}
}

class COpenVRContext
{
public:
	COpenVRContext() { Clear(); }
	void Clear();

	inline void CheckClear()
	{
		if (VRToken() != VR_GetInitToken())
		{
			Clear();
			VRToken() = VR_GetInitToken();
		}
	}

	IVRSystem* VRSystem()
	{
		CheckClear();
		if (m_pVRSystem == nullptr)
		{
			EVRInitError eError;
			m_pVRSystem = (IVRSystem*)VR_GetGenericInterface(IVRSystem_Version, &eError);
			outputError(IVRSystem_Version, eError);
		}
		return m_pVRSystem;
	}
	IVRChaperone* VRChaperone()
	{
		CheckClear();
		if (m_pVRChaperone == nullptr)
		{
			EVRInitError eError;
			m_pVRChaperone = (IVRChaperone*)VR_GetGenericInterface(IVRChaperone_Version, &eError);
			outputError(IVRChaperone_Version, eError);
		}
		return m_pVRChaperone;
	}

	IVRChaperoneSetup* VRChaperoneSetup()
	{
		CheckClear();
		if (m_pVRChaperoneSetup == nullptr)
		{
			EVRInitError eError;
			m_pVRChaperoneSetup = (IVRChaperoneSetup*)VR_GetGenericInterface(IVRChaperoneSetup_Version, &eError);
			outputError(IVRChaperoneSetup_Version, eError);
		}
		return m_pVRChaperoneSetup;
	}

	IVRCompositor* VRCompositor()
	{
		CheckClear();
		if (m_pVRCompositor == nullptr)
		{
			EVRInitError eError;
			m_pVRCompositor = (IVRCompositor*)VR_GetGenericInterface(IVRCompositor_Version, &eError);
			outputError(IVRCompositor_Version, eError);
			Log() << m_pVRCompositor << "\n";
		}
		return m_pVRCompositor;
	}

	IVROverlay* VROverlay()
	{
		CheckClear();
		if (m_pVROverlay == nullptr)
		{
			EVRInitError eError;
			m_pVROverlay = (IVROverlay*)VR_GetGenericInterface(IVROverlay_Version, &eError);
			outputError(IVROverlay_Version, eError);
		}
		return m_pVROverlay;
	}

	//IVROverlayView* VROverlayView()
	//{
	//	CheckClear();
	//	if (m_pVROverlayView == nullptr)
	//	{
	//		EVRInitError eError;
	//		m_pVROverlayView = (IVROverlayView*)VR_GetGenericInterface(IVROverlayView_Version, &eError);
	//		outputError(IVROverlayView_Version, eError);
	//	}
	//	return m_pVROverlayView;
	//}

	//IVRHeadsetView* VRHeadsetView()
	//{
	//	CheckClear();
	//	if (m_pVRHeadsetView == nullptr)
	//	{
	//		EVRInitError eError;
	//		m_pVRHeadsetView = (IVRHeadsetView*)VR_GetGenericInterface(IVRHeadsetView_Version, &eError);
	//		outputError(IVRHeadsetView_Version, eError);
	//	}
	//	return m_pVRHeadsetView;
	//}

	//IVRResources* VRResources()
	//{
	//	CheckClear();
	//	if (m_pVRResources == nullptr)
	//	{
	//		EVRInitError eError;
	//		m_pVRResources = (IVRResources*)VR_GetGenericInterface(IVRResources_Version, &eError);
	//		outputError(IVRResources_Version, eError);
	//	}
	//	return m_pVRResources;
	//}

	//IVRScreenshots* VRScreenshots()
	//{
	//	CheckClear();
	//	if (m_pVRScreenshots == nullptr)
	//	{
	//		EVRInitError eError;
	//		m_pVRScreenshots = (IVRScreenshots*)VR_GetGenericInterface(IVRScreenshots_Version, &eError);
	//		outputError(IVRScreenshots_Version, eError);
	//	}
	//	return m_pVRScreenshots;
	//}

	IVRRenderModels* VRRenderModels()
	{
		CheckClear();
		if (m_pVRRenderModels == nullptr)
		{
			EVRInitError eError;
			m_pVRRenderModels = (IVRRenderModels*)VR_GetGenericInterface(IVRRenderModels_Version, &eError);
			outputError(IVRRenderModels_Version, eError);
		}
		return m_pVRRenderModels;
	}

	IVRExtendedDisplay* VRExtendedDisplay()
	{
		CheckClear();
		if (m_pVRExtendedDisplay == nullptr)
		{
			EVRInitError eError;
			m_pVRExtendedDisplay = (IVRExtendedDisplay*)VR_GetGenericInterface(IVRExtendedDisplay_Version, &eError);
			outputError(IVRExtendedDisplay_Version, eError);
		}
		return m_pVRExtendedDisplay;
	}

	IVRSettings* VRSettings()
	{
		CheckClear();
		if (m_pVRSettings == nullptr)
		{
			EVRInitError eError;
			m_pVRSettings = (IVRSettings*)VR_GetGenericInterface(IVRSettings_Version, &eError);
			outputError(IVRSettings_Version, eError);
		}
		return m_pVRSettings;
	}

	IVRApplications* VRApplications()
	{
		CheckClear();
		if (m_pVRApplications == nullptr)
		{
			EVRInitError eError;
			m_pVRApplications = (IVRApplications*)VR_GetGenericInterface(IVRApplications_Version, &eError);
			outputError(IVRApplications_Version, eError);
		}
		return m_pVRApplications;
	}

	IVRTrackedCamera* VRTrackedCamera()
	{
		CheckClear();
		if (m_pVRTrackedCamera == nullptr)
		{
			EVRInitError eError;
			m_pVRTrackedCamera = (IVRTrackedCamera*)VR_GetGenericInterface(IVRTrackedCamera_Version, &eError);
			outputError(IVRTrackedCamera_Version, eError);
		}
		return m_pVRTrackedCamera;
	}

	//IVRDriverManager* VRDriverManager()
	//{
	//	CheckClear();
	//	if (!m_pVRDriverManager)
	//	{
	//		EVRInitError eError;
	//		m_pVRDriverManager = (IVRDriverManager*)VR_GetGenericInterface(IVRDriverManager_Version, &eError);
	//		outputError(IVRDriverManager_Version, eError);
	//	}
	//	return m_pVRDriverManager;
	//}

	//IVRInput* VRInput()
	//{
	//	CheckClear();
	//	if (!m_pVRInput)
	//	{
	//		EVRInitError eError;
	//		m_pVRInput = (IVRInput*)VR_GetGenericInterface(IVRInput_Version, &eError);
	//		outputError(IVRInput_Version, eError);
	//	}
	//	return m_pVRInput;
	//}

	//IVRIOBuffer* VRIOBuffer()
	//{
	//	if (!m_pVRIOBuffer)
	//	{
	//		EVRInitError eError;
	//		m_pVRIOBuffer = (IVRIOBuffer*)VR_GetGenericInterface(IVRIOBuffer_Version, &eError);
	//		outputError(IVRIOBuffer_Version, eError);
	//	}
	//	return m_pVRIOBuffer;
	//}

	//IVRSpatialAnchors* VRSpatialAnchors()
	//{
	//	CheckClear();
	//	if (!m_pVRSpatialAnchors)
	//	{
	//		EVRInitError eError;
	//		m_pVRSpatialAnchors = (IVRSpatialAnchors*)VR_GetGenericInterface(IVRSpatialAnchors_Version, &eError);
	//		outputError(IVRSpatialAnchors_Version, eError);
	//	}
	//	return m_pVRSpatialAnchors;
	//}

	//IVRDebug* VRDebug()
	//{
	//	CheckClear();
	//	if (!m_pVRDebug)
	//	{
	//		EVRInitError eError;
	//		m_pVRDebug = (IVRDebug*)VR_GetGenericInterface(IVRDebug_Version, &eError);
	//		outputError(IVRDebug_Version, eError);
	//	}
	//	return m_pVRDebug;
	//}

	IVRNotifications* VRNotifications()
	{
		CheckClear();
		if (!m_pVRNotifications)
		{
			EVRInitError eError;
			m_pVRNotifications = (IVRNotifications*)VR_GetGenericInterface(IVRNotifications_Version, &eError);
			outputError(IVRNotifications_Version, eError);
		}
		return m_pVRNotifications;
	}

private:
	IVRSystem* m_pVRSystem;
	IVRChaperone* m_pVRChaperone;
	IVRChaperoneSetup* m_pVRChaperoneSetup;
	IVRCompositor* m_pVRCompositor;
	//IVRHeadsetView* m_pVRHeadsetView;
	IVROverlay* m_pVROverlay;
	//IVROverlayView* m_pVROverlayView;
	//IVRResources* m_pVRResources;
	IVRRenderModels* m_pVRRenderModels;
	IVRExtendedDisplay* m_pVRExtendedDisplay;
	IVRSettings* m_pVRSettings;
	IVRApplications* m_pVRApplications;
	IVRTrackedCamera* m_pVRTrackedCamera;
	//IVRScreenshots* m_pVRScreenshots;
	//IVRDriverManager* m_pVRDriverManager;
	//IVRInput* m_pVRInput;
	//IVRIOBuffer* m_pVRIOBuffer;
	//IVRSpatialAnchors* m_pVRSpatialAnchors;
	//IVRDebug* m_pVRDebug;
	IVRNotifications* m_pVRNotifications;
};

inline COpenVRContext& OpenVRInternal_ModuleContext()
{
	static void* ctx[sizeof(COpenVRContext) / sizeof(void*)];
	return *(COpenVRContext*)ctx; // bypass zero-init constructor
}

VR_INTERFACE IVRSystem* VR_CALLTYPE VRSystem();
VR_INTERFACE IVRChaperone* VR_CALLTYPE VRChaperone();
VR_INTERFACE IVRChaperoneSetup* VR_CALLTYPE VRChaperoneSetup();
VR_INTERFACE IVRCompositor* VR_CALLTYPE VRCompositor();
VR_INTERFACE IVROverlay* VR_CALLTYPE VROverlay();
//VR_INTERFACE IVROverlayView* VR_CALLTYPE VROverlayView();
//VR_INTERFACE IVRHeadsetView* VR_CALLTYPE VRHeadsetView();
//VR_INTERFACE IVRScreenshots* VR_CALLTYPE VRScreenshots();
VR_INTERFACE IVRRenderModels* VR_CALLTYPE VRRenderModels();
VR_INTERFACE IVRApplications* VR_CALLTYPE VRApplications();
VR_INTERFACE IVRSettings* VR_CALLTYPE VRSettings();
//VR_INTERFACE IVRResources* VR_CALLTYPE VRResources();
VR_INTERFACE IVRExtendedDisplay* VR_CALLTYPE VRExtendedDisplay();
VR_INTERFACE IVRTrackedCamera* VR_CALLTYPE VRTrackedCamera();
//VR_INTERFACE IVRDriverManager* VR_CALLTYPE VRDriverManager();
//VR_INTERFACE IVRInput* VR_CALLTYPE VRInput();
//VR_INTERFACE IVRIOBuffer* VR_CALLTYPE VRIOBuffer();
//VR_INTERFACE IVRSpatialAnchors* VR_CALLTYPE VRSpatialAnchors();
VR_INTERFACE IVRNotifications* VR_CALLTYPE VRNotifications();
//VR_INTERFACE IVRDebug* VR_CALLTYPE VRDebug();

inline IVRSystem* VR_CALLTYPE VRSystem() { return OpenVRInternal_ModuleContext().VRSystem(); }
inline IVRChaperone* VR_CALLTYPE VRChaperone() { return OpenVRInternal_ModuleContext().VRChaperone(); }
inline IVRChaperoneSetup* VR_CALLTYPE VRChaperoneSetup() { return OpenVRInternal_ModuleContext().VRChaperoneSetup(); }
inline IVRCompositor* VR_CALLTYPE VRCompositor() { return OpenVRInternal_ModuleContext().VRCompositor(); }
inline IVROverlay* VR_CALLTYPE VROverlay() { return OpenVRInternal_ModuleContext().VROverlay(); }
//inline IVROverlayView* VR_CALLTYPE VROverlayView() { return OpenVRInternal_ModuleContext().VROverlayView(); }
//inline IVRHeadsetView* VR_CALLTYPE VRHeadsetView() { return OpenVRInternal_ModuleContext().VRHeadsetView(); }
//inline IVRScreenshots* VR_CALLTYPE VRScreenshots() { return OpenVRInternal_ModuleContext().VRScreenshots(); }
inline IVRRenderModels* VR_CALLTYPE VRRenderModels() { return OpenVRInternal_ModuleContext().VRRenderModels(); }
inline IVRApplications* VR_CALLTYPE VRApplications() { return OpenVRInternal_ModuleContext().VRApplications(); }
inline IVRSettings* VR_CALLTYPE VRSettings() { return OpenVRInternal_ModuleContext().VRSettings(); }
//inline IVRResources* VR_CALLTYPE VRResources() { return OpenVRInternal_ModuleContext().VRResources(); }
inline IVRExtendedDisplay* VR_CALLTYPE VRExtendedDisplay() { return OpenVRInternal_ModuleContext().VRExtendedDisplay(); }
inline IVRTrackedCamera* VR_CALLTYPE VRTrackedCamera() { return OpenVRInternal_ModuleContext().VRTrackedCamera(); }
//inline IVRDriverManager* VR_CALLTYPE VRDriverManager() { return OpenVRInternal_ModuleContext().VRDriverManager(); }
//inline IVRInput* VR_CALLTYPE VRInput() { return OpenVRInternal_ModuleContext().VRInput(); }
//inline IVRIOBuffer* VR_CALLTYPE VRIOBuffer() { return OpenVRInternal_ModuleContext().VRIOBuffer(); }
//inline IVRSpatialAnchors* VR_CALLTYPE VRSpatialAnchors() { return OpenVRInternal_ModuleContext().VRSpatialAnchors(); }
inline IVRNotifications* VR_CALLTYPE VRNotifications() { return OpenVRInternal_ModuleContext().VRNotifications(); }
//inline IVRDebug* VR_CALLTYPE VRDebug() { return OpenVRInternal_ModuleContext().VRDebug(); }

inline void COpenVRContext::Clear()
{
	m_pVRSystem = nullptr;
	m_pVRChaperone = nullptr;
	m_pVRChaperoneSetup = nullptr;
	m_pVRCompositor = nullptr;
	m_pVROverlay = nullptr;
	//m_pVROverlayView = nullptr;
	//m_pVRHeadsetView = nullptr;
	m_pVRRenderModels = nullptr;
	m_pVRExtendedDisplay = nullptr;
	m_pVRSettings = nullptr;
	m_pVRApplications = nullptr;
	m_pVRTrackedCamera = nullptr;
	//m_pVRResources = nullptr;
	//m_pVRScreenshots = nullptr;
	//m_pVRDriverManager = nullptr;
	//m_pVRInput = nullptr;
	//m_pVRIOBuffer = nullptr;
	//m_pVRSpatialAnchors = nullptr;
	m_pVRNotifications = nullptr;
	//m_pVRDebug = nullptr;
}

//VR_INTERFACE vr::IVRSystem* VR_CALLTYPE VR_Init(vr::EVRInitError* peError, vr::EVRApplicationType eApplicationType, const char* pStartupInfo);
/** Finds the active installation of vrclient.dll and initializes it */
inline IVRSystem* VR_Init(EVRInitError* peError, EVRApplicationType eApplicationType)
{
	IVRSystem* pVRSystem = nullptr;

	EVRInitError eError;
	VRToken() = VR_InitInternal2(&eError, eApplicationType, NULL);
	COpenVRContext& ctx = OpenVRInternal_ModuleContext();
	ctx.Clear();

	if (eError == VRInitError_None)
	{
		Log() << "1\n";
		if (VR_IsInterfaceVersionValid(IVRSystem_Version))
		{
			Log() << "2\n";
			pVRSystem = VRSystem();
		}
		else
		{
			Log() << "3\n";
			VR_ShutdownInternal();
			eError = VRInitError_Init_InterfaceNotFound;
		}
	}

	if (peError)
		*peError = eError;
	Log() << "4\n";
	return pVRSystem;
}

/** unloads vrclient.dll. Any interface pointers from the interface are
* invalid after this point */
inline void VR_Shutdown()
{
	VR_ShutdownInternal();
}


}

