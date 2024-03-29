/*************************************************************************/ /*!
@File
@Title          Common linux module setup
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include <linux/module.h>

#include "pvr_debugfs.h"
#include "private_data.h"
#include "linkage.h"
#include "lists.h"
#include "power.h"
#include "env_connection.h"
#include "process_stats.h"
#include "module_common.h"
#include "pvrsrv.h"
#include "srvcore.h"
#include "rgxdevice.h"
#include "pvrsrv_error.h"
#include "pvr_drv.h"

#include <pvr_fence.h>

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
#include "pvr_sync.h"
#endif

#if defined(SUPPORT_BUFFER_SYNC)
#include "pvr_buffer_sync.h"
#endif

#if defined(SUPPORT_GPUTRACE_EVENTS)
#include "pvr_gputrace.h"
#endif

#include "km_apphint.h"
#include "srvinit.h"

#if defined(SUPPORT_DISPLAY_CLASS)
/* Display class interface */
#include "kerneldisplay.h"
EXPORT_SYMBOL(DCRegisterDevice);
EXPORT_SYMBOL(DCUnregisterDevice);
EXPORT_SYMBOL(DCDisplayConfigurationRetired);
EXPORT_SYMBOL(DCDisplayHasPendingCommand);
EXPORT_SYMBOL(DCImportBufferAcquire);
EXPORT_SYMBOL(DCImportBufferRelease);

/* Physmem interface (required by LMA DC drivers) */
#include "physheap.h"
EXPORT_SYMBOL(PhysHeapAcquire);
EXPORT_SYMBOL(PhysHeapRelease);
EXPORT_SYMBOL(PhysHeapGetType);
EXPORT_SYMBOL(PhysHeapRegionGetCpuPAddr);
EXPORT_SYMBOL(PhysHeapRegionGetSize);
EXPORT_SYMBOL(PhysHeapCpuPAddrToDevPAddr);

EXPORT_SYMBOL(PVRSRVGetDriverStatus);
EXPORT_SYMBOL(PVRSRVSystemInstallDeviceLISR);
EXPORT_SYMBOL(PVRSRVSystemUninstallDeviceLISR);

#include "pvr_notifier.h"
EXPORT_SYMBOL(PVRSRVCheckStatus);

#include "pvr_debug.h"
EXPORT_SYMBOL(PVRSRVGetErrorStringKM);
#endif /* defined(SUPPORT_DISPLAY_CLASS) */

#include "rgxapi_km.h"
#if defined(SUPPORT_SHARED_SLC)
EXPORT_SYMBOL(RGXInitSLC);
#endif

#if defined(SUPPORT_KERNEL_HWPERF_TEST)
EXPORT_SYMBOL(OSAddTimer);
EXPORT_SYMBOL(OSEnableTimer);
EXPORT_SYMBOL(OSDisableTimer);
EXPORT_SYMBOL(OSRemoveTimer);
#endif

CONNECTION_DATA *LinuxConnectionFromFile(struct file *pFile)
{
	if (pFile)
	{
		struct drm_file *psDRMFile = pFile->private_data;

		return psDRMFile->driver_priv;
	}

	return NULL;
}

struct file *LinuxFileFromConnection(CONNECTION_DATA *psConnection)
{
	ENV_CONNECTION_DATA *psEnvConnection;

	psEnvConnection = PVRSRVConnectionPrivateData(psConnection);
	PVR_ASSERT(psEnvConnection != NULL);

	return psEnvConnection->psFile;
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDriverInit
@Description  Common one time driver initialisation
@Return       int           0 on success and a Linux error code otherwise
*/ /***************************************************************************/
int PVRSRVCommonDriverInit(void)
{
	PVRSRV_ERROR pvrerr;
	int error = 0;

#if defined(PDUMP)
	error = dbgdrv_init();
	if (error != 0)
	{
		return error;
	}
#endif

	error = PVRDebugFSInit();
	if (error != 0)
	{
		return error;
	}

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
	if (PVRSRVStatsInitialise() != PVRSRV_OK)
	{
		return -ENOMEM;
	}
#endif

	if (PVROSFuncInit() != PVRSRV_OK)
	{
		return -ENOMEM;
	}

	LinuxBridgeInit();

	error = pvr_apphint_init();
	if (error != 0)
	{
		PVR_DPF((PVR_DBG_WARNING,
			 "%s: failed AppHint setup(%d)",
			 __func__, error));
	}

	pvrerr = PVRSRVDriverInit();
	if (pvrerr != PVRSRV_OK)
	{
		return -ENODEV;
	}

	return 0;
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDriverDeinit
@Description  Common one time driver de-initialisation
@Return       void
*/ /***************************************************************************/
void PVRSRVCommonDriverDeinit(void)
{
	PVRSRVDriverDeInit();

	pvr_apphint_deinit();

	LinuxBridgeDeInit();

	PVROSFuncDeInit();

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
	PVRSRVStatsDestroy();
#endif
	PVRDebugFSDeInit();

#if defined(PDUMP)
	dbgdrv_cleanup();
#endif
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceInit
@Description  Common device related initialisation.
@Input        psDeviceNode  The device node for which initialisation should be
                            performed
@Return       int           0 on success and a Linux error code otherwise
*/ /***************************************************************************/
int PVRSRVCommonDeviceInit(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	int error = 0;
	PVRSRV_RGXDEV_INFO *psDevInfo = (PVRSRV_RGXDEV_INFO *) psDeviceNode->pvDevice;

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	{
		PVRSRV_ERROR eError = pvr_sync_init(psDeviceNode);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: unable to create sync (%d)",
					 __func__, eError));
			return -EBUSY;
		}
	}
#endif

#if defined(SUPPORT_BUFFER_SYNC)
	psDeviceNode->psBufferSyncContext =
		pvr_buffer_sync_context_create(psDeviceNode);
	if (IS_ERR(psDeviceNode->psBufferSyncContext))
	{
		error = PTR_ERR(psDeviceNode->psBufferSyncContext);
		psDeviceNode->psBufferSyncContext = NULL;

		PVR_DPF((PVR_DBG_ERROR,
				 "%s: unable to initialise buffer_sync support (%d)",
				 __func__, error));
		return error;
	}
#endif

	error = PVRDebugCreateDebugFSEntries();
	if (error != 0)
	{
		PVR_DPF((PVR_DBG_WARNING,
			 "%s: failed to create default debugfs entries (%d)",
			 __func__, error));
	}

#if defined(SUPPORT_GPUTRACE_EVENTS)
	error = PVRGpuTraceInitDevice(psDeviceNode);
	if (error != 0)
	{
		PVR_DPF((PVR_DBG_WARNING,
			 "%s: failed to initialise PVR GPU Tracing on device%d (%d)",
			 __func__, psDeviceNode->sDevId.i32UMIdentifier, error));
	}
#endif

	/* register the AppHint device control before device initialisation
	 * so individual AppHints can be configured during the init phase
	 */
	error = pvr_apphint_device_register(psDeviceNode);
	if (error != 0)
	{
		PVR_DPF((PVR_DBG_WARNING,
			 "%s: failed to initialise device AppHints (%d)",
			 __func__, error));
	}

	/*Initialize the device dependent bridges */
	error = DeviceDepBridgeInit(psDevInfo->sDevFeatureCfg.ui64Features);
	if (error != 0)
	{
		PVR_DPF((PVR_DBG_WARNING,
			 "%s: Device dependent bridge initialization failed (%d)",
			 __func__, error));
	}

	return 0;
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceDeinit
@Description  Common device related de-initialisation.
@Input        psDeviceNode  The device node for which de-initialisation should
                            be performed
@Return       void
*/ /***************************************************************************/
void PVRSRVCommonDeviceDeinit(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	int error = 0;
	PVRSRV_RGXDEV_INFO *psDevInfo = (PVRSRV_RGXDEV_INFO *) psDeviceNode->pvDevice;

	pvr_apphint_device_unregister(psDeviceNode);

#if defined(SUPPORT_GPUTRACE_EVENTS)
	PVRGpuTraceDeInitDevice(psDeviceNode);
#endif

	PVRDebugRemoveDebugFSEntries();

#if defined(SUPPORT_BUFFER_SYNC)
	pvr_buffer_sync_context_destroy(psDeviceNode->psBufferSyncContext);
#endif

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	pvr_sync_deinit();
#endif

	pvr_fence_cleanup();

	error = DeviceDepBridgeDeInit(psDevInfo->sDevFeatureCfg.ui64Features);
	if (error != 0)
	{
		PVR_DPF((PVR_DBG_WARNING,
			 "%s: Device dependent bridge deinitialization failed (%d)",
			 __func__, error));
	}
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceShutdown
@Description  Common device shutdown.
@Input        psDeviceNode  The device node representing the device that should
                            be shutdown
@Return       void
*/ /***************************************************************************/

void PVRSRVCommonDeviceShutdown(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVRSRV_ERROR eError;

	/*
	 * Disable the bridge to stop processes trying to use the driver
	 * after it has been shut down.
	 */
	eError = PVRSRVSuspendDriver();

	if(eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,
			"%s: Failed to suspend driver (%d)",
			__func__, eError));
		return;
	}

	(void) PVRSRVSetDeviceSystemPowerState(psDeviceNode,
										   PVRSRV_SYS_POWER_STATE_OFF);
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceSuspend
@Description  Common device suspend.
@Input        psDeviceNode  The device node representing the device that should
                            be suspended
@Return       int           0 on success and a Linux error code otherwise
*/ /***************************************************************************/
int PVRSRVCommonDeviceSuspend(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	/*
	 * PVRSRVSuspendDriver prevents processes from using the driver while it's
	 * suspended (this is needed for Android). Acquire the bridge lock first to
	 * ensure the driver isn't currently in use.
	 */

	PVRSRVSuspendDriver();

	if (PVRSRVSetDeviceSystemPowerState(psDeviceNode,
										PVRSRV_SYS_POWER_STATE_OFF) != PVRSRV_OK)
	{
		PVRSRVUnsuspendDriver();
		return -EINVAL;
	}

	return 0;
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceResume
@Description  Common device resume.
@Input        psDeviceNode  The device node representing the device that should
                            be resumed
@Return       int           0 on success and a Linux error code otherwise
*/ /***************************************************************************/
int PVRSRVCommonDeviceResume(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	if (PVRSRVSetDeviceSystemPowerState(psDeviceNode,
										PVRSRV_SYS_POWER_STATE_ON) != PVRSRV_OK)
	{
		return -EINVAL;
	}

	PVRSRVUnsuspendDriver();

	/*
	 * Reprocess the device queues in case commands were blocked during
	 * suspend.
	 */
	if (psDeviceNode->eDevState == PVRSRV_DEVICE_STATE_ACTIVE)
	{
		PVRSRVCheckStatus(NULL);
	}

	return 0;
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceOpen
@Description  Common device open.
@Input        psDeviceNode  The device node representing the device being
                            opened by a user mode process
@Input        psDRMFile     The DRM file data that backs the file handle
                            returned to the user mode process
@Return       int           0 on success and a Linux error code otherwise
*/ /***************************************************************************/
int PVRSRVCommonDeviceOpen(PVRSRV_DEVICE_NODE *psDeviceNode,
						   struct drm_file *psDRMFile)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	ENV_CONNECTION_PRIVATE_DATA sPrivData;
	void *pvConnectionData;
	PVRSRV_ERROR eError;
	int iErr = 0;

#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSAcquireBridgeLock();
#endif

	if (!psPVRSRVData)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: No device data", __func__));
		iErr = -ENODEV;
		goto e1;
	}

	/* 
	 * If the first attempt already set the state to bad,
	 * there is no point in going the second time, so get out 
	 */
	if (psDeviceNode->eDevState == PVRSRV_DEVICE_STATE_BAD)
	{    
		PVR_DPF((PVR_DBG_ERROR, "%s: Driver already in bad state. Device open failed.",
				 __func__));
		iErr = -ENODEV;
		goto e1; 
	}

	if (psDeviceNode->eDevState == PVRSRV_DEVICE_STATE_INIT)
	{
		eError = PVRSRVDeviceInitialise(psDeviceNode);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Failed to initialise device (%s)",
					 __func__, PVRSRVGetErrorStringKM(eError)));
			iErr = -ENODEV;
			goto e1;
		}

#if defined(SUPPORT_GPUTRACE_EVENTS)
		if (PVRGpuTraceEnabled())
		{
			PVRSRV_ERROR eError = PVRGpuTraceEnabledSetNoBridgeLock(psDeviceNode,
			                                                        IMG_TRUE);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "Failed to initialise GPU event tracing"
						" (%s)", PVRSRVGetErrorStringKM(eError)));
			}
		}
#endif
	}

	sPrivData.psDevNode = psDeviceNode;
	sPrivData.psFile = psDRMFile->filp;

	/*
	 * Here we pass the file pointer which will passed through to our
	 * OSConnectionPrivateDataInit function where we can save it so
	 * we can back reference the file structure from it's connection
	 */
	eError = PVRSRVConnectionConnect(&pvConnectionData, (void *) &sPrivData);
	if (eError != PVRSRV_OK)
	{
		iErr = -ENOMEM;
		goto e1;
	}

	psDRMFile->driver_priv = pvConnectionData;
#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSReleaseBridgeLock();
#endif

out:
	return iErr;
e1:
#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSReleaseBridgeLock();
#endif
	goto out;
}

/**************************************************************************/ /*!
@Function     PVRSRVCommonDeviceRelease
@Description  Common device release.
@Input        psDeviceNode  The device node for the device that the given file
                            represents
@Input        psDRMFile     The DRM file data that's being released
@Return       void
*/ /***************************************************************************/
void PVRSRVCommonDeviceRelease(PVRSRV_DEVICE_NODE *psDeviceNode,
							   struct drm_file *psDRMFile)
{
	void *pvConnectionData;

	PVR_UNREFERENCED_PARAMETER(psDeviceNode);

#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSAcquireBridgeLock();
#endif

	pvConnectionData = psDRMFile->driver_priv;
	if (pvConnectionData)
	{
		PVRSRVConnectionDisconnect(pvConnectionData);
		psDRMFile->driver_priv = NULL;
	}

#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSReleaseBridgeLock();
#endif
}
