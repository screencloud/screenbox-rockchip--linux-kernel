/*************************************************************************/ /*!
@File
@Title          RGX HW Performance implementation
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    RGX HW Performance implementation
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

//#define PVR_DPF_FUNCTION_TRACE_ON 1
#undef PVR_DPF_FUNCTION_TRACE_ON

#include "pvr_debug.h"
#include "rgxdevice.h"
#include "pvrsrv_error.h"
#include "pvr_notifier.h"
#include "osfunc.h"
#include "allocmem.h"

#include "pvrsrv.h"
#include "pvrsrv_tlstreams.h"
#include "pvrsrv_tlcommon.h"
#include "tlclient.h"
#include "tlstream.h"

#include "rgxhwperf.h"
#include "rgxapi_km.h"
#include "rgxfwutils.h"
#include "rgxtimecorr.h"
#include "devicemem.h"
#include "devicemem_pdump.h"
#include "pdump_km.h"
#include "pvrsrv_apphint.h"

#if defined(SUPPORT_GPUTRACE_EVENTS)
#include "pvr_gputrace.h"
#endif

/* This is defined by default to enable producer callbacks.
 * Clients of the TL interface can disable the use of the callback
 * with PVRSRV_STREAM_FLAG_DISABLE_PRODUCER_CALLBACK. */
#define SUPPORT_TL_PRODUCER_CALLBACK 1

/* Maximum enum value to prevent access to RGX_HWPERF_STREAM_ID2_CLIENT stream */
#define RGX_HWPERF_MAX_STREAM_ID (RGX_HWPERF_STREAM_ID2_CLIENT)

/* Defines size of buffers returned from acquire/release calls */
#define FW_STREAM_BUFFER_SIZE (0x80000)
#define HOST_STREAM_BUFFER_SIZE (0x20000)

/* Must be at least as large as two tl packets of maximum size */
static_assert(HOST_STREAM_BUFFER_SIZE >= (PVRSRVTL_MAX_PACKET_SIZE<<1),
			  "HOST_STREAM_BUFFER_SIZE is less than (PVRSRVTL_MAX_PACKET_SIZE<<1)");
static_assert(FW_STREAM_BUFFER_SIZE >= (PVRSRVTL_MAX_PACKET_SIZE<<1),
			  "FW_STREAM_BUFFER_SIZE is less than (PVRSRVTL_MAX_PACKET_SIZE<<1)");


/*
	RGXHWPerfCopyDataL1toL2
*/
static IMG_UINT32 RGXHWPerfCopyDataL1toL2(IMG_HANDLE hHWPerfStream,
										  IMG_BYTE   *pbFwBuffer,
										  IMG_UINT32 ui32BytesExp)
{
  	IMG_BYTE 	 *pbL2Buffer;
	IMG_UINT32   ui32L2BufFree;
	IMG_UINT32   ui32BytesCopied = 0;
	IMG_UINT32   ui32BytesExpMin = RGX_HWPERF_GET_SIZE(RGX_HWPERF_GET_PACKET(pbFwBuffer));
	PVRSRV_ERROR eError;

/* HWPERF_MISR_FUNC_DEBUG enables debug code for investigating HWPerf issues */
#ifdef HWPERF_MISR_FUNC_DEBUG
	static IMG_UINT32 gui32Ordinal = IMG_UINT32_MAX;
#endif

	PVR_DPF_ENTERED;

#ifdef HWPERF_MISR_FUNC_DEBUG
	PVR_DPF((PVR_DBG_VERBOSE, "EVENTS to copy from 0x%p length:%05d",
							  pbFwBuffer, ui32BytesExp));
#endif

#ifdef HWPERF_MISR_FUNC_DEBUG
	{
		/* Check the incoming buffer of data has not lost any packets */
 	 	IMG_BYTE *pbFwBufferIter = pbFwBuffer;
 	 	IMG_BYTE *pbFwBufferEnd = pbFwBuffer+ui32BytesExp;
	 	do
		{
			RGX_HWPERF_V2_PACKET_HDR *asCurPos = RGX_HWPERF_GET_PACKET(pbFwBufferIter);
			IMG_UINT32 ui32CurOrdinal = asCurPos->ui32Ordinal;
			if (gui32Ordinal != IMG_UINT32_MAX)
			{
				if ((gui32Ordinal+1) != ui32CurOrdinal)
				{
					if (gui32Ordinal < ui32CurOrdinal)
					{
						PVR_DPF((PVR_DBG_WARNING,
								 "HWPerf [%p] packets lost (%u packets) between ordinal %u...%u",
								 pbFwBufferIter,
								 ui32CurOrdinal - gui32Ordinal - 1,
								 gui32Ordinal,
								 ui32CurOrdinal));
					}
					else
					{
						PVR_DPF((PVR_DBG_WARNING,
								 "HWPerf [%p] packet ordinal out of sequence last: %u, current: %u",
								  pbFwBufferIter,
								  gui32Ordinal,
								  ui32CurOrdinal));
					}
				}
			}
			gui32Ordinal = asCurPos->ui32Ordinal;
			pbFwBufferIter += RGX_HWPERF_GET_SIZE(asCurPos);
		} while( pbFwBufferIter < pbFwBufferEnd );
	}
#endif

	/* Try submitting all data in one TL packet. */
	eError = TLStreamReserve2( hHWPerfStream,
							   &pbL2Buffer,
							   (size_t)ui32BytesExp, ui32BytesExpMin,
							   &ui32L2BufFree);
	if ( eError == PVRSRV_OK )
	{
		OSDeviceMemCopy( pbL2Buffer, pbFwBuffer, (size_t)ui32BytesExp );
		eError = TLStreamCommit(hHWPerfStream, (size_t)ui32BytesExp);
		if ( eError != PVRSRV_OK )
		{
			PVR_DPF((PVR_DBG_ERROR,
					 "TLStreamCommit() failed (%d) in %s(), unable to copy packet from L1 to L2 buffer",
					 eError, __func__));
			goto e0;
		}
		/* Data were successfully written */
		ui32BytesCopied = ui32BytesExp;
	}
	else if (eError == PVRSRV_ERROR_STREAM_RESERVE_TOO_BIG)
	{
		/* There was not enough space for all data, copy as much as possible */
		IMG_UINT32                sizeSum  = 0;
		RGX_PHWPERF_V2_PACKET_HDR psCurPkt = RGX_HWPERF_GET_PACKET(pbFwBuffer);

		PVR_DPF((PVR_DBG_MESSAGE, "Unable to reserve space (%d) in host buffer on first attempt, remaining free space: %d", ui32BytesExp, ui32L2BufFree));

		/* Traverse the array to find how many packets will fit in the available space. */
		while ( sizeSum < ui32BytesExp  &&
				sizeSum + RGX_HWPERF_GET_SIZE(psCurPkt) < ui32L2BufFree )
		{
			sizeSum += RGX_HWPERF_GET_SIZE(psCurPkt);
			psCurPkt = RGX_HWPERF_GET_NEXT_PACKET(psCurPkt);
		}

		if ( 0 != sizeSum )
		{
			eError = TLStreamReserve( hHWPerfStream, &pbL2Buffer, (size_t)sizeSum);

			if ( eError == PVRSRV_OK )
			{
				OSDeviceMemCopy( pbL2Buffer, pbFwBuffer, (size_t)sizeSum );
				eError = TLStreamCommit(hHWPerfStream, (size_t)sizeSum);
				if ( eError != PVRSRV_OK )
				{
					PVR_DPF((PVR_DBG_ERROR,
							 "TLStreamCommit() failed (%d) in %s(), unable to copy packet from L1 to L2 buffer",
							 eError, __func__));
					goto e0;
				}
				/* sizeSum bytes of hwperf packets have been successfully written */
				ui32BytesCopied = sizeSum;
			}
			else if ( PVRSRV_ERROR_STREAM_RESERVE_TOO_BIG == eError )
			{
				PVR_DPF((PVR_DBG_WARNING, "Cannot write HWPerf packet into host buffer, check data in case of packet loss, remaining free space: %d", ui32L2BufFree));
			}
		}
		else
		{
			PVR_DPF((PVR_DBG_MESSAGE, "Cannot find space in host buffer, check data in case of packet loss, remaining free space: %d", ui32L2BufFree));
		}
	}
	if ( PVRSRV_OK != eError && /* Some other error occurred */
	     PVRSRV_ERROR_STREAM_RESERVE_TOO_BIG != eError ) /* Full error handled by caller, we returning the copied bytes count to caller*/
	{
		PVR_DPF((PVR_DBG_ERROR,
				 "HWPerf enabled: Unexpected Error ( %d ) while copying FW buffer to TL buffer.",
				 eError));
	}

e0:
	/* Return the remaining packets left to be transported. */
	PVR_DPF_RETURN_VAL(ui32BytesCopied);
}


static INLINE IMG_UINT32 RGXHWPerfAdvanceRIdx(
		const IMG_UINT32 ui32BufSize,
		const IMG_UINT32 ui32Pos,
		const IMG_UINT32 ui32Size)
{
	return ( ui32Pos + ui32Size < ui32BufSize ? ui32Pos + ui32Size : 0 );
}


/*
	RGXHWPerfDataStore
*/
static IMG_UINT32 RGXHWPerfDataStore(PVRSRV_RGXDEV_INFO	*psDevInfo)
{
	RGXFWIF_TRACEBUF	*psRGXFWIfTraceBufCtl = psDevInfo->psRGXFWIfTraceBuf;
	IMG_BYTE*			psHwPerfInfo = psDevInfo->psRGXFWIfHWPerfBuf;
	IMG_UINT32			ui32SrcRIdx, ui32SrcWIdx, ui32SrcWrapCount;
	IMG_UINT32			ui32BytesExp = 0, ui32BytesCopied = 0, ui32BytesCopiedSum = 0;
#ifdef HWPERF_MISR_FUNC_DEBUG
	IMG_UINT32			ui32BytesExpSum = 0;
#endif

	PVR_DPF_ENTERED;

	/* Caller should check this member is valid before calling */
	PVR_ASSERT(psDevInfo->hHWPerfStream);

 	/* Get a copy of the current
	 *   read (first packet to read)
	 *   write (empty location for the next write to be inserted)
	 *   WrapCount (size in bytes of the buffer at or past end)
	 * indexes of the FW buffer */
	ui32SrcRIdx = psRGXFWIfTraceBufCtl->ui32HWPerfRIdx;
	ui32SrcWIdx = psRGXFWIfTraceBufCtl->ui32HWPerfWIdx;
	OSMemoryBarrier();
	ui32SrcWrapCount = psRGXFWIfTraceBufCtl->ui32HWPerfWrapCount;

	/* Is there any data in the buffer not yet retrieved? */
	if ( ui32SrcRIdx != ui32SrcWIdx )
	{
		PVR_DPF((PVR_DBG_MESSAGE, "RGXHWPerfDataStore EVENTS found srcRIdx:%d srcWIdx: %d ", ui32SrcRIdx, ui32SrcWIdx));

		/* Is the write position higher than the read position? */
		if ( ui32SrcWIdx > ui32SrcRIdx )
		{
			/* Yes, buffer has not wrapped */
			ui32BytesExp = ui32SrcWIdx - ui32SrcRIdx;
#ifdef HWPERF_MISR_FUNC_DEBUG
			ui32BytesExpSum += ui32BytesExp;
#endif
			ui32BytesCopied = RGXHWPerfCopyDataL1toL2(psDevInfo->hHWPerfStream,
													  psHwPerfInfo + ui32SrcRIdx,
													  ui32BytesExp);

			/* Advance the read index and the free bytes counter by the number
			 * of bytes transported. Items will be left in buffer if not all data
			 * could be transported. Exit to allow buffer to drain. */
			psRGXFWIfTraceBufCtl->ui32HWPerfRIdx = RGXHWPerfAdvanceRIdx(
					psDevInfo->ui32RGXFWIfHWPerfBufSize, ui32SrcRIdx,
					ui32BytesCopied);

			ui32BytesCopiedSum += ui32BytesCopied;
		}
		/* No, buffer has wrapped and write position is behind read position */
		else
		{
			/* Byte count equal to
			 *     number of bytes from read position to the end of the buffer,
			 *   + data in the extra space in the end of the buffer. */
			ui32BytesExp = ui32SrcWrapCount - ui32SrcRIdx;

#ifdef HWPERF_MISR_FUNC_DEBUG
			ui32BytesExpSum += ui32BytesExp;
#endif
			/* Attempt to transfer the packets to the TL stream buffer */
			ui32BytesCopied = RGXHWPerfCopyDataL1toL2(psDevInfo->hHWPerfStream,
													  psHwPerfInfo + ui32SrcRIdx,
													  ui32BytesExp);

			/* Advance read index as before and Update the local copy of the
			 * read index as it might be used in the last if branch*/
			ui32SrcRIdx = RGXHWPerfAdvanceRIdx(
					psDevInfo->ui32RGXFWIfHWPerfBufSize, ui32SrcRIdx,
					ui32BytesCopied);

			/* Update Wrap Count */
			if ( ui32SrcRIdx == 0)
			{
				psRGXFWIfTraceBufCtl->ui32HWPerfWrapCount = psDevInfo->ui32RGXFWIfHWPerfBufSize;
			}
			psRGXFWIfTraceBufCtl->ui32HWPerfRIdx = ui32SrcRIdx;

			ui32BytesCopiedSum += ui32BytesCopied;

			/* If all the data in the end of the array was copied, try copying
			 * wrapped data in the beginning of the array, assuming there is
			 * any and the RIdx was wrapped. */
			if (   (ui32BytesCopied == ui32BytesExp)
			    && (ui32SrcWIdx > 0)
				&& (ui32SrcRIdx == 0) )
			{
				ui32BytesExp = ui32SrcWIdx;
#ifdef HWPERF_MISR_FUNC_DEBUG
				ui32BytesExpSum += ui32BytesExp;
#endif
				ui32BytesCopied = RGXHWPerfCopyDataL1toL2(psDevInfo->hHWPerfStream,
														  psHwPerfInfo,
														  ui32BytesExp);
				/* Advance the FW buffer read position. */
				psRGXFWIfTraceBufCtl->ui32HWPerfRIdx = RGXHWPerfAdvanceRIdx(
						psDevInfo->ui32RGXFWIfHWPerfBufSize, ui32SrcRIdx,
						ui32BytesCopied);

				ui32BytesCopiedSum += ui32BytesCopied;
			}
		}
#ifdef HWPERF_MISR_FUNC_DEBUG
		if (ui32BytesCopiedSum != ui32BytesExpSum)
		{
			PVR_DPF((PVR_DBG_WARNING, "RGXHWPerfDataStore: FW L1 RIdx:%u. Not all bytes copied to L2: %u bytes out of %u expected", psRGXFWIfTraceBufCtl->ui32HWPerfRIdx, ui32BytesCopiedSum, ui32BytesExpSum));
		}
#endif

	}
	else
	{
		PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfDataStore NO EVENTS to transport"));
	}

	PVR_DPF_RETURN_VAL(ui32BytesCopiedSum);
}


PVRSRV_ERROR RGXHWPerfDataStoreCB(PVRSRV_DEVICE_NODE *psDevInfo)
{
	PVRSRV_ERROR		eError = PVRSRV_OK;
	PVRSRV_RGXDEV_INFO* psRgxDevInfo;
	IMG_UINT32          ui32BytesCopied;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_OK);

	PVR_DPF_ENTERED;

	PVR_ASSERT(psDevInfo);
	psRgxDevInfo = psDevInfo->pvDevice;

	/* Keep HWPerf resource init check and use of
	 * resources atomic, they may not be freed during use
	 */
	OSLockAcquire(psRgxDevInfo->hHWPerfLock);

	if (psRgxDevInfo->hHWPerfStream != NULL)
	{
		ui32BytesCopied = RGXHWPerfDataStore(psRgxDevInfo);
		if ( ui32BytesCopied )
		{	/* Signal consumers that packets may be available to read when
			 * running from a HW kick, not when called by client APP thread
			 * via the transport layer CB as this can lead to stream
			 * corruption.*/
			eError = TLStreamSync(psRgxDevInfo->hHWPerfStream);
			PVR_ASSERT(eError == PVRSRV_OK);
		}
		else
		{
			PVR_DPF((PVR_DBG_MESSAGE, "RGXHWPerfDataStoreCB: Zero bytes copied"));
			RGXDEBUG_PRINT_IRQ_COUNT(psRgxDevInfo);
		}
	}

	OSLockRelease(psRgxDevInfo->hHWPerfLock);

	PVR_DPF_RETURN_OK;
}


/* Currently supported by default */
#if defined(SUPPORT_TL_PRODUCER_CALLBACK)
static PVRSRV_ERROR RGXHWPerfTLCB(IMG_HANDLE hStream,
		IMG_UINT32 ui32ReqOp, IMG_UINT32* ui32Resp, void* pvUser)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_RGXDEV_INFO* psRgxDevInfo = (PVRSRV_RGXDEV_INFO*)pvUser;

	PVR_UNREFERENCED_PARAMETER(hStream);
	PVR_UNREFERENCED_PARAMETER(ui32Resp);

	PVR_ASSERT(psRgxDevInfo);

	switch (ui32ReqOp)
	{
	case TL_SOURCECB_OP_CLIENT_EOS:
		/* Keep HWPerf resource init check and use of
		 * resources atomic, they may not be freed during use
		 */

		/* This solution is for avoiding a deadlock situation where -
		 * in DoTLStreamReserve(), writer has acquired HWPerfLock and
		 * ReadLock and is waiting on ReadPending (which will be reset
		 * by reader), And
		 * the reader after setting ReadPending in TLStreamAcquireReadPos(),
		 * is waiting for HWPerfLock in RGXHWPerfTLCB().
		 * So here in RGXHWPerfTLCB(), if HWPerfLock is already acquired we
		 * will return to the reader without waiting to acquire HWPerfLock.
		 */
		if( !OSTryLockAcquire(psRgxDevInfo->hHWPerfLock))
		{
			PVR_DPF((PVR_DBG_MESSAGE, "hHWPerfLock is already acquired, a write "
						  "operation might already be in process"));
			return PVRSRV_OK;
		}

		if (psRgxDevInfo->hHWPerfStream != NULL)
		{
			(void) RGXHWPerfDataStore(psRgxDevInfo);
		}
		OSLockRelease(psRgxDevInfo->hHWPerfLock);
		break;

	default:
		break;
	}

	return eError;
}
#endif


static void RGXHWPerfL1BufferDeinit(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	if (psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc)
	{
		if (psRgxDevInfo->psRGXFWIfHWPerfBuf != NULL)
		{
			DevmemReleaseCpuVirtAddr(psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc);
			psRgxDevInfo->psRGXFWIfHWPerfBuf = NULL;
		}
		DevmemFwFree(psRgxDevInfo, psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc);
		psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc = NULL;
	}
}

/*************************************************************************/ /*!
@Function       RGXHWPerfInit

@Description    Called during driver init for initialization of HWPerf module
				in the Rogue device driver. This function keeps allocated
				only the minimal necessary resources, which are required for
				functioning of HWPerf server module.

@Input          psRgxDevInfo	RGX Device Info

@Return			PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR RGXHWPerfInit(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVRSRV_ERROR eError;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_OK);

	PVR_DPF_ENTERED;

	/* expecting a valid device info */
	PVR_ASSERT(psRgxDevInfo);

	/* Create a lock for HWPerf server module used for serializing, L1 to L2
	 * copy calls (e.g. in case of TL producer callback) and L1, L2 resource
	 * allocation */
	eError = OSLockCreate(&psRgxDevInfo->hHWPerfLock, LOCK_TYPE_PASSIVE);
	PVR_LOGR_IF_ERROR(eError, "OSLockCreate");

	/* avoid uninitialised data */
	psRgxDevInfo->hHWPerfStream = NULL;
	psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc = NULL;

	PVR_DPF_RETURN_OK;
}

/*************************************************************************/ /*!
@Function       RGXHWPerfIsInitRequired

@Description    Returns true if the HWperf firmware buffer (L1 buffer) and host
                driver TL buffer (L2 buffer) are not already allocated. Caller
                must possess hHWPerfLock lock before calling this
                function so the state tested is not inconsistent.

@Input          psRgxDevInfo RGX Device Info, on which init requirement is
                checked.

@Return         IMG_BOOL	Whether initialization (allocation) is required
*/ /**************************************************************************/
static INLINE IMG_BOOL RGXHWPerfIsInitRequired(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVR_ASSERT(OSLockIsLocked(psRgxDevInfo->hHWPerfLock));

#if !defined (NO_HARDWARE)
	/* Both L1 and L2 buffers are required (for HWPerf functioning) on driver
	 * built for actual hardware (TC, EMU, etc.)
	 */
	if (psRgxDevInfo->hHWPerfStream == NULL)
	{
		/* The allocation API (RGXHWPerfInitOnDemandResources) allocates
		 * device memory for both L1 and L2 without any checks. Hence,
		 * either both should be allocated or both be NULL.
		 *
		 * In-case this changes in future (for e.g. a situation where one
		 * of the 2 buffers is already allocated and other is required),
		 * add required checks before allocation calls to avoid memory leaks.
		 */
		PVR_ASSERT(psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc == NULL);
		return IMG_TRUE;
	}
	PVR_ASSERT(psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc != NULL);
#else
	/* On a NO-HW driver L2 is not allocated. So, no point in checking its
	 * allocation */
	if (psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc == NULL)
	{
		return IMG_TRUE;
	}
#endif
	return IMG_FALSE;
}

/*************************************************************************/ /*!
@Function       RGXHWPerfInitOnDemandResources

@Description    This function allocates the HWperf firmware buffer (L1 buffer)
                and host driver TL buffer (L2 buffer) if HWPerf is enabled at
                driver load time. Otherwise, these buffers are allocated
                on-demand as and when required. Caller
                must possess hHWPerfLock lock before calling this
                function so the state tested is not inconsistent if called
                outside of driver initialisation.

@Input          psRgxDevInfo RGX Device Info, on which init is done

@Return         PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR RGXHWPerfInitOnDemandResources(PVRSRV_RGXDEV_INFO* psRgxDevInfo)
{
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32L2BufferSize;
	DEVMEM_FLAGS_T uiMemAllocFlags;
	IMG_CHAR pszHWPerfStreamName[sizeof(PVRSRV_TL_HWPERF_RGX_FW_STREAM) + 5]; /* 5 seems reasonable as it can hold
																			  names up to "hwperf_9999", which is enough */

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	PVR_DPF_ENTERED;

	/* Create the L1 HWPerf buffer on demand */
	uiMemAllocFlags = PVRSRV_MEMALLOCFLAG_DEVICE_FLAG(PMMETA_PROTECT)
				| PVRSRV_MEMALLOCFLAG_GPU_READABLE
				| PVRSRV_MEMALLOCFLAG_GPU_WRITEABLE
				| PVRSRV_MEMALLOCFLAG_CPU_READABLE
				| PVRSRV_MEMALLOCFLAG_KERNEL_CPU_MAPPABLE
				| PVRSRV_MEMALLOCFLAG_UNCACHED
				#if defined(PDUMP)
				| PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC
				#endif
				;

	/* Allocate HWPerf FW L1 buffer */
	eError = DevmemFwAllocate(psRgxDevInfo,
							  psRgxDevInfo->ui32RGXFWIfHWPerfBufSize+RGXFW_HWPERF_L1_PADDING_DEFAULT,
							  uiMemAllocFlags,
							  "FwHWPerfBuffer",
							  &psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate kernel fw hwperf buffer (%u)",
					__FUNCTION__, eError));
		goto e0;
	}

	/* Expecting the RuntimeCfg structure is mapped into CPU virtual memory.
	 * Also, make sure the FW address is not already set */
	PVR_ASSERT(psRgxDevInfo->psRGXFWIfRuntimeCfg && psRgxDevInfo->psRGXFWIfRuntimeCfg->sHWPerfBuf.ui32Addr == 0x0);

	/* Meta cached flag removed from this allocation as it was found
	 * FW performance was better without it. */
	RGXSetFirmwareAddress(&psRgxDevInfo->psRGXFWIfRuntimeCfg->sHWPerfBuf,
						  psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc,
						  0, RFW_FWADDR_NOREF_FLAG);

	eError = DevmemAcquireCpuVirtAddr(psRgxDevInfo->psRGXFWIfHWPerfBufMemDesc,
									  (void**)&psRgxDevInfo->psRGXFWIfHWPerfBuf);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to acquire kernel hwperf buffer (%u)",
					 __FUNCTION__, eError));
		goto e0;
	}

	/* On NO-HW driver, there is no MISR installed to copy data from L1 to L2. Hence,
	 * L2 buffer is not allocated */
#if !defined(NO_HARDWARE)
	/* Host L2 HWPERF buffer size in bytes must be bigger than the L1 buffer
	 * accessed by the FW. The MISR may try to write one packet the size of the L1
	 * buffer in some scenarios. When logging is enabled in the MISR, it can be seen
	 * if the L2 buffer hits a full condition. The closer in size the L2 and L1 buffers
	 * are the more chance of this happening.
	 * Size chosen to allow MISR to write an L1 sized packet and for the client
	 * application/daemon to drain a L1 sized packet e.g. ~ 1.5*L1.
	 */
	ui32L2BufferSize = psRgxDevInfo->ui32RGXFWIfHWPerfBufSize +
	                       (psRgxDevInfo->ui32RGXFWIfHWPerfBufSize>>1);

	/* form the HWPerf stream name, corresponding to this DevNode; which can make sense in the UM */
	if (OSSNPrintf(pszHWPerfStreamName, sizeof(pszHWPerfStreamName), "%s%d",
				   PVRSRV_TL_HWPERF_RGX_FW_STREAM,
				   psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier) < 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to form HWPerf stream name for device %d",
		                        __FUNCTION__,
								psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	eError = TLStreamCreate(&psRgxDevInfo->hHWPerfStream,
					psRgxDevInfo->psDeviceNode,
					pszHWPerfStreamName,
					ui32L2BufferSize,
					TL_OPMODE_DROP_NEWER | TL_FLAG_NO_SIGNAL_ON_COMMIT,
					NULL, NULL,
#if !defined(SUPPORT_TL_PRODUCER_CALLBACK)
					NULL, NULL
#else
					/* Not enabled by default */
					RGXHWPerfTLCB, psRgxDevInfo
#endif
					);
	PVR_LOGG_IF_ERROR(eError, "TLStreamCreate", e1);

	eError = TLStreamSetNotifStream(psRgxDevInfo->hHWPerfStream,
	                                PVRSRVGetPVRSRVData()->hTLCtrlStream);
	/* we can still discover host stream so leave it as is and just log error */
	PVR_LOG_IF_ERROR(eError, "TLStreamSetNotifStream");

	/* send the event here because host stream is implicitly opened for write
	 * in TLStreamCreate and TLStreamOpen is never called (so the event is
	 * never emitted) */
	TLStreamMarkStreamOpen(psRgxDevInfo->hHWPerfStream);

	PVR_DPF((PVR_DBG_MESSAGE, "HWPerf buffer size in bytes: L1: %d  L2: %d",
			psRgxDevInfo->ui32RGXFWIfHWPerfBufSize, ui32L2BufferSize));

#else /* defined (NO_HARDWARE) */
	PVR_UNREFERENCED_PARAMETER(ui32L2BufferSize);
	PVR_UNREFERENCED_PARAMETER(RGXHWPerfTLCB);
	PVR_UNREFERENCED_PARAMETER(pszHWPerfStreamName);
ui32L2BufferSize = 0;
#endif

	PVR_DPF_RETURN_OK;

#if !defined(NO_HARDWARE)
e1: /* L2 buffer initialisation failures */
	psRgxDevInfo->hHWPerfStream = NULL;
#endif
e0: /* L1 buffer initialisation failures */
	RGXHWPerfL1BufferDeinit(psRgxDevInfo);

	PVR_DPF_RETURN_RC(eError);
}


void RGXHWPerfDeinit(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVRSRV_VZ_RETN_IF_MODE(DRIVER_MODE_GUEST);

	PVR_DPF_ENTERED;

	PVR_ASSERT(psRgxDevInfo);

	/* Clean up the L2 buffer stream object if allocated */
	if (psRgxDevInfo->hHWPerfStream)
	{
		/* send the event here because host stream is implicitly opened for
		 * write in TLStreamCreate and TLStreamClose is never called (so the
		 * event is never emitted) */
		TLStreamMarkStreamClose(psRgxDevInfo->hHWPerfStream);
		TLStreamClose(psRgxDevInfo->hHWPerfStream);
		psRgxDevInfo->hHWPerfStream = NULL;
	}

	/* Cleanup L1 buffer resources */
	RGXHWPerfL1BufferDeinit(psRgxDevInfo);

	/* Cleanup the HWPerf server module lock resource */
	if (psRgxDevInfo->hHWPerfLock)
	{
		OSLockDestroy(psRgxDevInfo->hHWPerfLock);
		psRgxDevInfo->hHWPerfLock = NULL;
	}

	PVR_DPF_RETURN;
}


/******************************************************************************
 * RGX HW Performance Profiling Server API(s)
 *****************************************************************************/

static PVRSRV_ERROR RGXHWPerfCtrlFwBuffer(const PVRSRV_DEVICE_NODE *psDeviceNode,
                                          IMG_BOOL bToggle,
                                          IMG_UINT64 ui64Mask)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_RGXDEV_INFO* psDevice = psDeviceNode->pvDevice;
	RGXFWIF_KCCB_CMD sKccbCmd;

	/* If this method is being used whether to enable or disable
	 * then the hwperf buffers (host and FW) are likely to be needed
	 * eventually so create them, also helps unit testing. Buffers
	 * allocated on demand to reduce RAM foot print on systems not
	 * needing HWPerf resources.
	 * Obtain lock first, test and init if required. */
	OSLockAcquire(psDevice->hHWPerfLock);

	if (!psDevice->bFirmwareInitialised)
	{
		psDevice->ui64HWPerfFilter = ui64Mask; // at least set filter
		eError = PVRSRV_ERROR_NOT_INITIALISED;

		PVR_DPF((PVR_DBG_ERROR, "HWPerf has NOT been initialised yet."
		        " Mask has been SET to (%llx)", (long long) ui64Mask));

		goto unlock_and_return;
	}

	if (RGXHWPerfIsInitRequired(psDevice))
	{
		eError = RGXHWPerfInitOnDemandResources(psDevice);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Initialisation of on-demand HWPerfFW "
			        "resources failed", __func__));
			goto unlock_and_return;
		}
	}

	/* Unlock here as no further HWPerf resources are used below that would be
	 * affected if freed by another thread */
	OSLockRelease(psDevice->hHWPerfLock);

	/* Return if the filter is the same */
	if (!bToggle && psDevice->ui64HWPerfFilter == ui64Mask)
		goto return_;

	/* Prepare command parameters ... */
	sKccbCmd.eCmdType = RGXFWIF_KCCB_CMD_HWPERF_UPDATE_CONFIG;
	sKccbCmd.uCmdData.sHWPerfCtrl.bToggle = bToggle;
	sKccbCmd.uCmdData.sHWPerfCtrl.ui64Mask = ui64Mask;

	/* Ask the FW to carry out the HWPerf configuration command */
	eError = RGXScheduleCommand(psDeviceNode->pvDevice,	RGXFWIF_DM_GP,
								&sKccbCmd, sizeof(sKccbCmd), 0, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to set new HWPerfFW filter in "
				"firmware (error = %d)", __func__, eError));
		goto return_;
	}

	psDevice->ui64HWPerfFilter = bToggle ?
	        psDevice->ui64HWPerfFilter ^ ui64Mask : ui64Mask;

	/* Wait for FW to complete */
	eError = RGXWaitForFWOp(psDeviceNode->pvDevice, RGXFWIF_DM_GP,
	                        psDeviceNode->psSyncPrim, PDUMP_FLAGS_CONTINUOUS);
	PVR_LOGG_IF_ERROR(eError, "RGXWaitForFWOp", return_);

#if defined(DEBUG)
	if (bToggle)
	{
		PVR_DPF((PVR_DBG_WARNING, "HWPerfFW events (%" IMG_UINT64_FMTSPECx ") have been TOGGLED",
		        ui64Mask));
	}
	else
	{
		PVR_DPF((PVR_DBG_WARNING, "HWPerfFW mask has been SET to (%" IMG_UINT64_FMTSPECx ")",
		        ui64Mask));
	}
#endif

	return PVRSRV_OK;

unlock_and_return:
	OSLockRelease(psDevice->hHWPerfLock);

return_:
	return eError;
}

static PVRSRV_ERROR RGXHWPerfCtrlHostBuffer(const PVRSRV_DEVICE_NODE *psDeviceNode,
                                            IMG_BOOL bToggle,
                                            IMG_UINT32 ui32Mask)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_RGXDEV_INFO* psDevice = psDeviceNode->pvDevice;

	OSLockAcquire(psDevice->hLockHWPerfHostStream);
	if (psDevice->hHWPerfHostStream == NULL)
	{
		eError = RGXHWPerfHostInitOnDemandResources(psDevice);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Initialization of on-demand HWPerfHost"
			        " resources failed", __FUNCTION__));
			OSLockRelease(psDevice->hLockHWPerfHostStream);
			return eError;
		}
	}

	psDevice->ui32HWPerfHostFilter = bToggle ?
	        psDevice->ui32HWPerfHostFilter ^ ui32Mask : ui32Mask;
	OSLockRelease(psDevice->hLockHWPerfHostStream);

#if defined(DEBUG)
	if (bToggle)
	{
		PVR_DPF((PVR_DBG_WARNING, "HWPerfHost events (%x) have been TOGGLED",
		        ui32Mask));
	}
	else
	{
		PVR_DPF((PVR_DBG_WARNING, "HWPerfHost mask has been SET to (%x)",
		        ui32Mask));
	}
#endif

	return PVRSRV_OK;
}

static PVRSRV_ERROR RGXHWPerfCtrlClientBuffer(IMG_BOOL bToggle,
                                              IMG_UINT32 ui32InfoPageIdx,
                                              IMG_UINT32 ui32Mask)
{
	PVRSRV_DATA *psData = PVRSRVGetPVRSRVData();

	PVR_LOGR_IF_FALSE(ui32InfoPageIdx >= HWPERF_INFO_IDX_START &&
	                  ui32InfoPageIdx < HWPERF_INFO_IDX_END, "invalid info"
	                  " page index", PVRSRV_ERROR_INVALID_PARAMS);

	OSLockAcquire(psData->hInfoPageLock);
	psData->pui32InfoPage[ui32InfoPageIdx] = bToggle ?
	        psData->pui32InfoPage[ui32InfoPageIdx] ^ ui32Mask : ui32Mask;
	OSLockRelease(psData->hInfoPageLock);

#if defined(DEBUG)
	if (bToggle)
	{
		PVR_DPF((PVR_DBG_WARNING, "HWPerfClient (%u) events (%x) have been TOGGLED",
		        ui32InfoPageIdx, ui32Mask));
	}
	else
	{
		PVR_DPF((PVR_DBG_WARNING, "HWPerfClient (%u) mask has been SET to (%x)",
		        ui32InfoPageIdx, ui32Mask));
	}
#endif

	return PVRSRV_OK;
}

/*
	PVRSRVRGXCtrlHWPerfKM
*/
PVRSRV_ERROR PVRSRVRGXCtrlHWPerfKM(
	CONNECTION_DATA         *psConnection,
	PVRSRV_DEVICE_NODE      *psDeviceNode,
	RGX_HWPERF_STREAM_ID     eStreamId,
	IMG_BOOL                 bToggle,
	IMG_UINT64               ui64Mask)
{
	PVR_UNREFERENCED_PARAMETER(psConnection);

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	PVR_DPF_ENTERED;
	PVR_ASSERT(psDeviceNode);

	if (eStreamId == RGX_HWPERF_STREAM_ID0_FW)
	{
		return RGXHWPerfCtrlFwBuffer(psDeviceNode, bToggle, ui64Mask);
	}
	else if (eStreamId == RGX_HWPERF_STREAM_ID1_HOST)
	{
		return RGXHWPerfCtrlHostBuffer(psDeviceNode, bToggle, (IMG_UINT32) ui64Mask);
	}
	else if (eStreamId == RGX_HWPERF_STREAM_ID2_CLIENT)
	{
		IMG_UINT32 ui32Index = (IMG_UINT32) (ui64Mask >> 32);
		IMG_UINT32 ui32Mask = (IMG_UINT32) ui64Mask;

		return RGXHWPerfCtrlClientBuffer(bToggle, ui32Index, ui32Mask);
	}
	else
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVRGXCtrlHWPerfKM: Unknown stream id."));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	PVR_DPF_RETURN_OK;
}

/*
	AppHint interfaces
*/
static
PVRSRV_ERROR RGXHWPerfSetFwFilter(const PVRSRV_DEVICE_NODE *psDeviceNode,
                                  const void *psPrivate,
                                  IMG_UINT64 ui64Value)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PVRSRV_DEVICE_NODE *psDevNode;
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
	PVR_UNREFERENCED_PARAMETER(psPrivate);

	psDevNode = psPVRSRVData->psDeviceNodeList;
	/* Control HWPerf on all the devices */
	while (psDevNode)
	{
		eError = RGXHWPerfCtrlFwBuffer(psDevNode, IMG_FALSE, ui64Value);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "Failed to set HWPerf firmware filter for device (%d)", psDevNode->sDevId.i32UMIdentifier));
			return eError;
		}
		psDevNode = psDevNode->psNext;
	}
	return PVRSRV_OK;
}

static
PVRSRV_ERROR RGXHWPerfReadFwFilter(const PVRSRV_DEVICE_NODE *psDeviceNode,
                                   const void *psPrivate,
                                   IMG_UINT64 *pui64Value)
{
	PVRSRV_RGXDEV_INFO *psDevice;

	PVR_UNREFERENCED_PARAMETER(psPrivate);

	if (!psDeviceNode || !psDeviceNode->pvDevice)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* Configuration command is applied for all devices, so filter value should
	 * be same for all */
	psDevice = psDeviceNode->pvDevice;
	*pui64Value = psDevice->ui64HWPerfFilter;
	return PVRSRV_OK;
}

static
PVRSRV_ERROR RGXHWPerfSetHostFilter(const PVRSRV_DEVICE_NODE *psDeviceNode,
                                    const void *psPrivate,
                                    IMG_UINT32 ui32Value)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PVRSRV_DEVICE_NODE *psDevNode;
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
	PVR_UNREFERENCED_PARAMETER(psPrivate);

	psDevNode = psPVRSRVData->psDeviceNodeList;
	/* Control HWPerf on all the devices */
	while (psDevNode)
	{
		eError = RGXHWPerfCtrlHostBuffer(psDevNode, IMG_FALSE, ui32Value);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "Failed to set HWPerf firmware filter for device (%d)", psDevNode->sDevId.i32UMIdentifier));
			return eError;
		}
		psDevNode = psDevNode->psNext;
	}
	return PVRSRV_OK;
}

static
PVRSRV_ERROR RGXHWPerfReadHostFilter(const PVRSRV_DEVICE_NODE *psDeviceNode,
                                     const void *psPrivate,
                                     IMG_UINT32 *pui32Value)
{
	PVRSRV_RGXDEV_INFO *psDevice;

	PVR_UNREFERENCED_PARAMETER(psPrivate);

	if (!psDeviceNode || !psDeviceNode->pvDevice)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevice = psDeviceNode->pvDevice;
	*pui32Value = psDevice->ui32HWPerfHostFilter;
	return PVRSRV_OK;
}

static PVRSRV_ERROR _ReadClientFilter(const PVRSRV_DEVICE_NODE *psDevice,
                                      const void *psPrivData,
                                      IMG_UINT32 *pui32Value)
{
	PVRSRV_DATA *psData = PVRSRVGetPVRSRVData();
	IMG_UINT32 ui32Idx = (IMG_UINT32) (uintptr_t) psPrivData;
	PVR_UNREFERENCED_PARAMETER(psDevice);

	OSLockAcquire(psData->hInfoPageLock);
	*pui32Value = psData->pui32InfoPage[ui32Idx];
	OSLockRelease(psData->hInfoPageLock);

	return PVRSRV_OK;
}

static PVRSRV_ERROR _WriteClientFilter(const PVRSRV_DEVICE_NODE *psDevice,
                                       const void *psPrivData,
                                       IMG_UINT32 ui32Value)
{
	IMG_UINT32 ui32Idx = (IMG_UINT32) (uintptr_t) psPrivData;
	PVR_UNREFERENCED_PARAMETER(psDevice);

	return RGXHWPerfCtrlClientBuffer(IMG_FALSE, ui32Idx, ui32Value);
}

void RGXHWPerfInitAppHintCallbacks(const PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVRSRVAppHintRegisterHandlersUINT64(APPHINT_ID_HWPerfFWFilter,
	                                    RGXHWPerfReadFwFilter,
	                                    RGXHWPerfSetFwFilter,
	                                    psDeviceNode,
	                                    NULL);
	PVRSRVAppHintRegisterHandlersUINT32(APPHINT_ID_HWPerfHostFilter,
	                                    RGXHWPerfReadHostFilter,
	                                    RGXHWPerfSetHostFilter,
	                                    psDeviceNode,
	                                    NULL);
}

void RGXHWPerfClientInitAppHintCallbacks(void)
{
	PVRSRVAppHintRegisterHandlersUINT32(APPHINT_ID_HWPerfClientFilter_Services,
	                                    _ReadClientFilter,
	                                    _WriteClientFilter,
	                                    APPHINT_OF_DRIVER_NO_DEVICE,
	                                    (void *) HWPERF_FILTER_SERVICES_IDX);
	PVRSRVAppHintRegisterHandlersUINT32(APPHINT_ID_HWPerfClientFilter_EGL,
	                                    _ReadClientFilter,
	                                    _WriteClientFilter,
	                                    APPHINT_OF_DRIVER_NO_DEVICE,
	                                    (void *) HWPERF_FILTER_EGL_IDX);
	PVRSRVAppHintRegisterHandlersUINT32(APPHINT_ID_HWPerfClientFilter_OpenGLES,
	                                    _ReadClientFilter,
	                                    _WriteClientFilter,
	                                    APPHINT_OF_DRIVER_NO_DEVICE,
	                                    (void *) HWPERF_FILTER_OPENGLES_IDX);
	PVRSRVAppHintRegisterHandlersUINT32(APPHINT_ID_HWPerfClientFilter_OpenCL,
	                                    _ReadClientFilter,
	                                    _WriteClientFilter,
	                                    APPHINT_OF_DRIVER_NO_DEVICE,
	                                    (void *) HWPERF_FILTER_OPENCL_IDX);
	PVRSRVAppHintRegisterHandlersUINT32(APPHINT_ID_HWPerfClientFilter_OpenRL,
	                                    _ReadClientFilter,
	                                    _WriteClientFilter,
	                                    APPHINT_OF_DRIVER_NO_DEVICE,
	                                    (void *) HWPERF_FILTER_OPENRL_IDX);
}

/*
	PVRSRVRGXEnableHWPerfCountersKM
*/
PVRSRV_ERROR PVRSRVRGXConfigEnableHWPerfCountersKM(
	CONNECTION_DATA          * psConnection,
	PVRSRV_DEVICE_NODE       * psDeviceNode,
	IMG_UINT32                 ui32ArrayLen,
	RGX_HWPERF_CONFIG_CNTBLK * psBlockConfigs)
{
	PVRSRV_ERROR 		eError = PVRSRV_OK;
	RGXFWIF_KCCB_CMD 	sKccbCmd;
	DEVMEM_MEMDESC*		psFwBlkConfigsMemDesc;
	RGX_HWPERF_CONFIG_CNTBLK* psFwArray;

	PVR_UNREFERENCED_PARAMETER(psConnection);

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	PVR_DPF_ENTERED;

	PVR_ASSERT(psDeviceNode);
	PVR_ASSERT(ui32ArrayLen>0);
	PVR_ASSERT(psBlockConfigs);

	/* Fill in the command structure with the parameters needed
	 */
	sKccbCmd.eCmdType = RGXFWIF_KCCB_CMD_HWPERF_CONFIG_ENABLE_BLKS;
	sKccbCmd.uCmdData.sHWPerfCfgEnableBlks.ui32NumBlocks = ui32ArrayLen;

	eError = DevmemFwAllocate(psDeviceNode->pvDevice,
			sizeof(RGX_HWPERF_CONFIG_CNTBLK)*ui32ArrayLen,
			PVRSRV_MEMALLOCFLAG_DEVICE_FLAG(PMMETA_PROTECT) |
									  PVRSRV_MEMALLOCFLAG_GPU_READABLE |
									  PVRSRV_MEMALLOCFLAG_GPU_WRITEABLE |
									  PVRSRV_MEMALLOCFLAG_CPU_READABLE |
									  PVRSRV_MEMALLOCFLAG_KERNEL_CPU_MAPPABLE |
									  PVRSRV_MEMALLOCFLAG_UNCACHED |
									  PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC,
			"FwHWPerfCountersConfigBlock",
			&psFwBlkConfigsMemDesc);
	if (eError != PVRSRV_OK)
		PVR_LOGR_IF_ERROR(eError, "DevmemFwAllocate");

	RGXSetFirmwareAddress(&sKccbCmd.uCmdData.sHWPerfCfgEnableBlks.sBlockConfigs,
			psFwBlkConfigsMemDesc, 0, 0);

	eError = DevmemAcquireCpuVirtAddr(psFwBlkConfigsMemDesc, (void **)&psFwArray);
	if (eError != PVRSRV_OK)
	{
		PVR_LOGG_IF_ERROR(eError, "DevmemAcquireCpuVirtAddr", fail1);
	}

	OSDeviceMemCopy(psFwArray, psBlockConfigs, sizeof(RGX_HWPERF_CONFIG_CNTBLK)*ui32ArrayLen);
	DevmemPDumpLoadMem(psFwBlkConfigsMemDesc,
						0,
						sizeof(RGX_HWPERF_CONFIG_CNTBLK)*ui32ArrayLen,
						0);

	/* PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXConfigEnableHWPerfCountersKM parameters set, calling FW")); */

	/* Ask the FW to carry out the HWPerf configuration command
	 */
	eError = RGXScheduleCommand(psDeviceNode->pvDevice,
			RGXFWIF_DM_GP, &sKccbCmd, sizeof(sKccbCmd), 0, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
	{
		PVR_LOGG_IF_ERROR(eError, "RGXScheduleCommand", fail2);
	}

	/* PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXConfigEnableHWPerfCountersKM command scheduled for FW")); */

	/* Wait for FW to complete */
	eError = RGXWaitForFWOp(psDeviceNode->pvDevice, RGXFWIF_DM_GP, psDeviceNode->psSyncPrim, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
	{
		PVR_LOGG_IF_ERROR(eError, "RGXWaitForFWOp", fail2);
	}

	/* Release temporary memory used for block configuration
	 */
	RGXUnsetFirmwareAddress(psFwBlkConfigsMemDesc);
	DevmemReleaseCpuVirtAddr(psFwBlkConfigsMemDesc);
	DevmemFwFree(psDeviceNode->pvDevice, psFwBlkConfigsMemDesc);

	/* PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXConfigEnableHWPerfCountersKM firmware completed")); */

	PVR_DPF((PVR_DBG_WARNING, "HWPerf %d counter blocks configured and ENABLED", ui32ArrayLen));

	PVR_DPF_RETURN_OK;

fail2:
	DevmemReleaseCpuVirtAddr(psFwBlkConfigsMemDesc);
fail1:
	RGXUnsetFirmwareAddress(psFwBlkConfigsMemDesc);
	DevmemFwFree(psDeviceNode->pvDevice, psFwBlkConfigsMemDesc);

	PVR_DPF_RETURN_RC(eError);
}


/*
	PVRSRVRGXConfigCustomCountersReadingHWPerfKM
 */
PVRSRV_ERROR PVRSRVRGXConfigCustomCountersKM(
	CONNECTION_DATA             * psConnection,
	PVRSRV_DEVICE_NODE          * psDeviceNode,
	IMG_UINT16                    ui16CustomBlockID,
	IMG_UINT16                    ui16NumCustomCounters,
	IMG_UINT32                  * pui32CustomCounterIDs)
{
	PVRSRV_ERROR		eError = PVRSRV_OK;
	RGXFWIF_KCCB_CMD	sKccbCmd;
	DEVMEM_MEMDESC*		psFwSelectCntrsMemDesc = NULL;
	IMG_UINT32*			psFwArray;

	PVR_UNREFERENCED_PARAMETER(psConnection);

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	PVR_DPF_ENTERED;

	PVR_ASSERT(psDeviceNode);

	PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVRGXSelectCustomCountersKM: configure block %u to read %u counters", ui16CustomBlockID, ui16NumCustomCounters));

	/* Fill in the command structure with the parameters needed */
	sKccbCmd.eCmdType = RGXFWIF_KCCB_CMD_HWPERF_SELECT_CUSTOM_CNTRS;
	sKccbCmd.uCmdData.sHWPerfSelectCstmCntrs.ui16NumCounters = ui16NumCustomCounters;
	sKccbCmd.uCmdData.sHWPerfSelectCstmCntrs.ui16CustomBlock = ui16CustomBlockID;

	if (ui16NumCustomCounters > 0)
	{
		PVR_ASSERT(pui32CustomCounterIDs);

		eError = DevmemFwAllocate(psDeviceNode->pvDevice,
				sizeof(IMG_UINT32) * ui16NumCustomCounters,
				PVRSRV_MEMALLOCFLAG_DEVICE_FLAG(PMMETA_PROTECT) |
				PVRSRV_MEMALLOCFLAG_GPU_READABLE |
				PVRSRV_MEMALLOCFLAG_GPU_WRITEABLE |
				PVRSRV_MEMALLOCFLAG_CPU_READABLE |
				PVRSRV_MEMALLOCFLAG_KERNEL_CPU_MAPPABLE |
				PVRSRV_MEMALLOCFLAG_UNCACHED |
				PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC,
				"FwHWPerfConfigCustomCounters",
				&psFwSelectCntrsMemDesc);
		if (eError != PVRSRV_OK)
			PVR_LOGR_IF_ERROR(eError, "DevmemFwAllocate");

		RGXSetFirmwareAddress(&sKccbCmd.uCmdData.sHWPerfSelectCstmCntrs.sCustomCounterIDs,
				psFwSelectCntrsMemDesc, 0, 0);

		eError = DevmemAcquireCpuVirtAddr(psFwSelectCntrsMemDesc, (void **)&psFwArray);
		if (eError != PVRSRV_OK)
		{
			PVR_LOGG_IF_ERROR(eError, "DevmemAcquireCpuVirtAddr", fail1);
		}

		OSDeviceMemCopy(psFwArray, pui32CustomCounterIDs, sizeof(IMG_UINT32) * ui16NumCustomCounters);
		DevmemPDumpLoadMem(psFwSelectCntrsMemDesc,
				0,
				sizeof(IMG_UINT32) * ui16NumCustomCounters,
				0);
	}

	/* Push in the KCCB the command to configure the custom counters block */
	eError = RGXScheduleCommand(psDeviceNode->pvDevice,
			RGXFWIF_DM_GP, &sKccbCmd, sizeof(sKccbCmd), 0, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
	{
		PVR_LOGG_IF_ERROR(eError, "RGXScheduleCommand", fail2);
	}
	PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXSelectCustomCountersKM: Command scheduled"));

	/* Wait for FW to complete */
	eError = RGXWaitForFWOp(psDeviceNode->pvDevice, RGXFWIF_DM_GP, psDeviceNode->psSyncPrim, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
	{
		PVR_LOGG_IF_ERROR(eError, "RGXWaitForFWOp", fail2);
	}
	PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXSelectCustomCountersKM: FW operation completed"));

	if (ui16NumCustomCounters > 0)
	{
		/* Release temporary memory used for block configuration */
		RGXUnsetFirmwareAddress(psFwSelectCntrsMemDesc);
		DevmemReleaseCpuVirtAddr(psFwSelectCntrsMemDesc);
		DevmemFwFree(psDeviceNode->pvDevice, psFwSelectCntrsMemDesc);
	}

	PVR_DPF((PVR_DBG_MESSAGE, "HWPerf custom counters %u reading will be sent with the next HW events", ui16NumCustomCounters));

	PVR_DPF_RETURN_OK;

	fail2:
	if (psFwSelectCntrsMemDesc) DevmemReleaseCpuVirtAddr(psFwSelectCntrsMemDesc);

	fail1:
	if (psFwSelectCntrsMemDesc)
	{
		RGXUnsetFirmwareAddress(psFwSelectCntrsMemDesc);
		DevmemFwFree(psDeviceNode->pvDevice, psFwSelectCntrsMemDesc);
	}

	PVR_DPF_RETURN_RC(eError);
}
/*
	PVRSRVRGXDisableHWPerfcountersKM
*/
PVRSRV_ERROR PVRSRVRGXCtrlHWPerfCountersKM(
	CONNECTION_DATA             * psConnection,
	PVRSRV_DEVICE_NODE          * psDeviceNode,
	IMG_BOOL                      bEnable,
	IMG_UINT32                    ui32ArrayLen,
	IMG_UINT16                  * psBlockIDs)
{
	PVRSRV_ERROR 		eError = PVRSRV_OK;
	RGXFWIF_KCCB_CMD 	sKccbCmd;

	PVR_UNREFERENCED_PARAMETER(psConnection);

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	PVR_DPF_ENTERED;

	PVR_ASSERT(psDeviceNode);
	PVR_ASSERT(ui32ArrayLen>0);
	PVR_ASSERT(ui32ArrayLen<=RGXFWIF_HWPERF_CTRL_BLKS_MAX);
	PVR_ASSERT(psBlockIDs);

	/* Fill in the command structure with the parameters needed
	 */
	sKccbCmd.eCmdType = RGXFWIF_KCCB_CMD_HWPERF_CTRL_BLKS;
	sKccbCmd.uCmdData.sHWPerfCtrlBlks.bEnable = bEnable;
	sKccbCmd.uCmdData.sHWPerfCtrlBlks.ui32NumBlocks = ui32ArrayLen;
	OSDeviceMemCopy(sKccbCmd.uCmdData.sHWPerfCtrlBlks.aeBlockIDs, psBlockIDs, sizeof(IMG_UINT16)*ui32ArrayLen);

	/* PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXCtrlHWPerfCountersKM parameters set, calling FW")); */

	/* Ask the FW to carry out the HWPerf configuration command
	 */
	eError = RGXScheduleCommand(psDeviceNode->pvDevice,
			RGXFWIF_DM_GP, &sKccbCmd, sizeof(sKccbCmd), 0, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
		PVR_LOGR_IF_ERROR(eError, "RGXScheduleCommand");

	/* PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXCtrlHWPerfCountersKM command scheduled for FW")); */

	/* Wait for FW to complete */
	eError = RGXWaitForFWOp(psDeviceNode->pvDevice, RGXFWIF_DM_GP, psDeviceNode->psSyncPrim, PDUMP_FLAGS_CONTINUOUS);
	if (eError != PVRSRV_OK)
		PVR_LOGR_IF_ERROR(eError, "RGXWaitForFWOp");

	/* PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVRGXCtrlHWPerfCountersKM firmware completed")); */

#if defined(DEBUG)
	if (bEnable)
		PVR_DPF((PVR_DBG_WARNING, "HWPerf %d counter blocks have been ENABLED", ui32ArrayLen));
	else
		PVR_DPF((PVR_DBG_WARNING, "HWPerf %d counter blocks have been DISABLED", ui32ArrayLen));
#endif

	PVR_DPF_RETURN_OK;
}

static INLINE IMG_UINT32 _RGXHWPerfFixBufferSize(IMG_UINT32 ui32BufSizeKB)
{
	if (ui32BufSizeKB > HWPERF_HOST_TL_STREAM_SIZE_MAX)
	{
		/* Size specified as a AppHint but it is too big */
		PVR_DPF((PVR_DBG_WARNING,"RGXHWPerfHostInit: HWPerf Host buffer size "
				"value (%u) too big, using maximum (%u)", ui32BufSizeKB,
		        HWPERF_HOST_TL_STREAM_SIZE_MAX));
		return HWPERF_HOST_TL_STREAM_SIZE_MAX<<10;
	}
	else if (ui32BufSizeKB >= HWPERF_HOST_TL_STREAM_SIZE_MIN)
	{
		return ui32BufSizeKB<<10;
	}
	else if (ui32BufSizeKB > 0)
	{
		/* Size specified as a AppHint but it is too small */
		PVR_DPF((PVR_DBG_WARNING,"RGXHWPerfHostInit: HWPerf Host buffer size "
		        "value (%u) too small, using minimum (%u)", ui32BufSizeKB,
		        HWPERF_HOST_TL_STREAM_SIZE_MIN));
		return HWPERF_HOST_TL_STREAM_SIZE_MIN<<10;
	}
	else
	{
		/* 0 size implies AppHint not set or is set to zero,
		 * use default size from driver constant. */
		return HWPERF_HOST_TL_STREAM_SIZE_DEFAULT<<10;
	}
}

/******************************************************************************
 * RGX HW Performance Host Stream API
 *****************************************************************************/

/*************************************************************************/ /*!
@Function       RGXHWPerfHostInit

@Description    Called during driver init for initialisation of HWPerfHost
                stream in the Rogue device driver. This function keeps allocated
                only the minimal necessary resources, which are required for
                functioning of HWPerf server module.

@Return         PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR RGXHWPerfHostInit(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_UINT32 ui32BufSizeKB)
{
	PVRSRV_ERROR eError;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_OK);

	PVR_ASSERT(psRgxDevInfo != NULL);

	eError = OSLockCreate(&psRgxDevInfo->hLockHWPerfHostStream, LOCK_TYPE_PASSIVE);
	PVR_LOGG_IF_ERROR(eError, "OSLockCreate", error);

	psRgxDevInfo->hHWPerfHostStream = NULL;
	psRgxDevInfo->ui32HWPerfHostFilter = 0; /* disable all events */
	psRgxDevInfo->ui32HWPerfHostNextOrdinal = 0;
	psRgxDevInfo->ui32HWPerfHostBufSize = _RGXHWPerfFixBufferSize(ui32BufSizeKB);

error:
	return eError;
}

static void _HWPerfHostOnConnectCB(void *pvArg)
{
	RGX_HWPERF_HOST_CLK_SYNC(pvArg);
}

/*************************************************************************/ /*!
@Function       RGXHWPerfHostInitOnDemandResources

@Description    This function allocates the HWPerfHost buffer if HWPerf is
                enabled at driver load time. Otherwise, these buffers are
                allocated on-demand as and when required.

@Return         PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR RGXHWPerfHostInitOnDemandResources(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVRSRV_ERROR eError;
	IMG_CHAR pszHWPerfHostStreamName[sizeof(PVRSRV_TL_HWPERF_HOST_SERVER_STREAM) + 5]; /* 5 makes space up to "hwperf_host_9999" streams */

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* form the HWPerf host stream name, corresponding to this DevNode; which can make sense in the UM */
	if (OSSNPrintf(pszHWPerfHostStreamName, sizeof(pszHWPerfHostStreamName), "%s%d",
				   PVRSRV_TL_HWPERF_HOST_SERVER_STREAM,
				   psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier) < 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to form HWPerf host stream name for device %d",
		                        __FUNCTION__,
								psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	eError = TLStreamCreate(&psRgxDevInfo->hHWPerfHostStream,
							psRgxDevInfo->psDeviceNode,
					        pszHWPerfHostStreamName, psRgxDevInfo->ui32HWPerfHostBufSize,
					        TL_OPMODE_DROP_NEWER,
							_HWPerfHostOnConnectCB, psRgxDevInfo,
							NULL, NULL);
	PVR_LOGG_IF_ERROR(eError, "TLStreamCreate", error_stream_create);

	eError = TLStreamSetNotifStream(psRgxDevInfo->hHWPerfHostStream,
	                                PVRSRVGetPVRSRVData()->hTLCtrlStream);
	/* we can still discover host stream so leave it as is and just log error */
	PVR_LOG_IF_ERROR(eError, "TLStreamSetNotifStream");

	/* send the event here because host stream is implicitly opened for write
	 * in TLStreamCreate and TLStreamOpen is never called (so the event is
	 * never emitted) */
	TLStreamMarkStreamOpen(psRgxDevInfo->hHWPerfHostStream);

	PVR_DPF((DBGPRIV_MESSAGE, "HWPerf Host buffer size is %uKB",
	        psRgxDevInfo->ui32HWPerfHostBufSize));

	return PVRSRV_OK;

error_stream_create:
	OSLockDestroy(psRgxDevInfo->hLockHWPerfHostStream);
	psRgxDevInfo->hLockHWPerfHostStream = NULL;

	return eError;
}

void RGXHWPerfHostDeInit(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVRSRV_VZ_RETN_IF_MODE(DRIVER_MODE_GUEST);

	PVR_ASSERT (psRgxDevInfo);
	if (psRgxDevInfo->hHWPerfHostStream)
	{
		/* send the event here because host stream is implicitly opened for
		 * write in TLStreamCreate and TLStreamClose is never called (so the
		 * event is never emitted) */
		TLStreamMarkStreamClose(psRgxDevInfo->hHWPerfHostStream);
		TLStreamClose(psRgxDevInfo->hHWPerfHostStream);
		psRgxDevInfo->hHWPerfHostStream = NULL;
	}

	if (psRgxDevInfo->hLockHWPerfHostStream)
	{
		OSLockDestroy(psRgxDevInfo->hLockHWPerfHostStream);
		psRgxDevInfo->hLockHWPerfHostStream = NULL;
	}
}

inline void RGXHWPerfHostSetEventFilter(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_UINT32 ui32Filter)
{
	PVRSRV_VZ_RETN_IF_MODE(DRIVER_MODE_GUEST);
	psRgxDevInfo->ui32HWPerfHostFilter = ui32Filter;
}

inline IMG_BOOL RGXHWPerfHostIsEventEnabled(PVRSRV_RGXDEV_INFO *psRgxDevInfo, RGX_HWPERF_HOST_EVENT_TYPE eEvent)
{
	PVR_ASSERT(psRgxDevInfo);
	return (psRgxDevInfo->ui32HWPerfHostFilter & RGX_HWPERF_EVENT_MASK_VALUE(eEvent)) ? IMG_TRUE : IMG_FALSE;
}

static inline void _PostFunctionPrologue(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVR_ASSERT(psRgxDevInfo->hLockHWPerfHostStream != NULL);
	PVR_ASSERT(psRgxDevInfo->hHWPerfHostStream != NULL);

	OSLockAcquire(psRgxDevInfo->hLockHWPerfHostStream);

	/* In case we drop packet we increment ordinal beforehand. */
	psRgxDevInfo->ui32HWPerfHostNextOrdinal++;
}

static inline void _PostFunctionEpilogue(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	OSLockRelease(psRgxDevInfo->hLockHWPerfHostStream);
}

static inline IMG_UINT8 *_ReserveHWPerfStream(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_UINT32 ui32Size)
{
	IMG_UINT8 *pui8Dest;

	PVRSRV_ERROR eError = TLStreamReserve(psRgxDevInfo->hHWPerfHostStream,
	                         &pui8Dest, ui32Size);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_MESSAGE, "%s: Could not reserve space in %s buffer"
		        " (%d). Dropping packet.",
		        __func__, PVRSRV_TL_HWPERF_HOST_SERVER_STREAM, eError));
		return NULL;
	}
	PVR_ASSERT(pui8Dest != NULL);

	return pui8Dest;
}

static inline void _CommitHWPerfStream(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_UINT32 ui32Size)
{
	PVRSRV_ERROR eError = TLStreamCommit(psRgxDevInfo->hHWPerfHostStream,
	                                     ui32Size);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_MESSAGE, "%s: Could not commit data to %s"
	            " (%d)", __func__, PVRSRV_TL_HWPERF_HOST_SERVER_STREAM, eError));
	}
}

static inline void _SetupHostPacketHeader(PVRSRV_RGXDEV_INFO *psRgxDevInfo,
                                          IMG_UINT8 *pui8Dest,
                                          RGX_HWPERF_HOST_EVENT_TYPE eEvType,
                                          IMG_UINT32 ui32Size)
{
	RGX_HWPERF_V2_PACKET_HDR *psHeader = (RGX_HWPERF_V2_PACKET_HDR *) pui8Dest;

	PVR_ASSERT(ui32Size<=RGX_HWPERF_MAX_PACKET_SIZE);

	psHeader->ui32Ordinal = psRgxDevInfo->ui32HWPerfHostNextOrdinal;
	psHeader->ui64Timestamp = RGXGPUFreqCalibrateClockus64();
	psHeader->ui32Sig = HWPERF_PACKET_V2B_SIG;
	psHeader->eTypeId = RGX_HWPERF_MAKE_TYPEID(RGX_HWPERF_STREAM_ID1_HOST,
	        eEvType, 0, 0);
	psHeader->ui32Size = ui32Size;
}

static inline void _SetupHostEnqPacketData(IMG_UINT8 *pui8Dest,
                                           RGX_HWPERF_KICK_TYPE eEnqType,
                                           IMG_UINT32 ui32Pid,
                                           IMG_UINT32 ui32FWDMContext,
                                           IMG_UINT32 ui32ExtJobRef,
                                           IMG_UINT32 ui32IntJobRef,
                                           IMG_UINT32 ui32CheckFenceUID,
                                           IMG_UINT32 ui32UpdateFenceUID,
                                           IMG_UINT64 ui64DeadlineInus,
                                           IMG_UINT64 ui64CycleEstimate)
{
	RGX_HWPERF_HOST_ENQ_DATA *psData = (RGX_HWPERF_HOST_ENQ_DATA *)
	        (pui8Dest + sizeof(RGX_HWPERF_V2_PACKET_HDR));
	psData->ui32EnqType = eEnqType;
	psData->ui32PID = ui32Pid;
	psData->ui32ExtJobRef = ui32ExtJobRef;
	psData->ui32IntJobRef = ui32IntJobRef;
	psData->ui32DMContext = ui32FWDMContext;
	psData->ui32Padding = 0;       /* Set to zero for future compatibility */
	psData->ui32CheckFence_UID = ui32CheckFenceUID;
	psData->ui32UpdateFence_UID = ui32UpdateFenceUID;
	psData->ui64DeadlineInus = ui64DeadlineInus;
	psData->ui64CycleEstimate = ui64CycleEstimate;
}

void RGXHWPerfHostPostEnqEvent(PVRSRV_RGXDEV_INFO *psRgxDevInfo,
                               RGX_HWPERF_KICK_TYPE eEnqType,
                               IMG_UINT32 ui32Pid,
                               IMG_UINT32 ui32FWDMContext,
                               IMG_UINT32 ui32ExtJobRef,
                               IMG_UINT32 ui32IntJobRef,
                               IMG_UINT32 ui32CheckFenceUID,
                               IMG_UINT32 ui32UpdateFenceUID,
                               IMG_UINT64 ui64DeadlineInus,
                               IMG_UINT64 ui64CycleEstimate )
{
	IMG_UINT8 *pui8Dest;
	IMG_UINT32 ui32Size = RGX_HWPERF_MAKE_SIZE_FIXED(RGX_HWPERF_HOST_ENQ_DATA);

	_PostFunctionPrologue(psRgxDevInfo);

	if ((pui8Dest = _ReserveHWPerfStream(psRgxDevInfo, ui32Size)) == NULL)
	{
		goto cleanup;
	}

	_SetupHostPacketHeader(psRgxDevInfo, pui8Dest, RGX_HWPERF_HOST_ENQ, ui32Size);
	_SetupHostEnqPacketData(pui8Dest,
	                        eEnqType,
	                        ui32Pid,
	                        ui32FWDMContext,
	                        ui32ExtJobRef,
	                        ui32IntJobRef,
	                        ui32CheckFenceUID,
	                        ui32UpdateFenceUID,
	                        ui64DeadlineInus,
	                        ui64CycleEstimate);

	_CommitHWPerfStream(psRgxDevInfo, ui32Size);

cleanup:
	_PostFunctionEpilogue(psRgxDevInfo);
}

static inline IMG_UINT32 _CalculateHostUfoPacketSize(RGX_HWPERF_UFO_EV eUfoType,
                                                     IMG_UINT uiNoOfUFOs)
{
	IMG_UINT32 ui32Size =
		(IMG_UINT32) offsetof(RGX_HWPERF_UFO_DATA, aui32StreamData);
	RGX_HWPERF_UFO_DATA_ELEMENT *puData;

	switch (eUfoType)
	{
		case RGX_HWPERF_UFO_EV_CHECK_SUCCESS:
		case RGX_HWPERF_UFO_EV_PRCHECK_SUCCESS:
			ui32Size += uiNoOfUFOs * sizeof(puData->sCheckSuccess);
			break;
		case RGX_HWPERF_UFO_EV_CHECK_FAIL:
		case RGX_HWPERF_UFO_EV_PRCHECK_FAIL:
			ui32Size += uiNoOfUFOs * sizeof(puData->sCheckFail);
			break;
		case RGX_HWPERF_UFO_EV_UPDATE:
			ui32Size += uiNoOfUFOs * sizeof(puData->sUpdate);
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR, "RGXHWPerfHostPostUfoEvent: Invalid UFO"
			        " event type"));
			PVR_ASSERT(IMG_FALSE);
			break;
	}

	return RGX_HWPERF_MAKE_SIZE_VARIABLE(ui32Size);
}

static inline void _SetupHostUfoPacketData(IMG_UINT8 *pui8Dest,
                                        RGX_HWPERF_UFO_EV eUfoType,
                                        RGX_HWPERF_UFO_DATA_ELEMENT psUFOData[],
                                        IMG_UINT uiNoOfUFOs)
{
	IMG_UINT uiUFOIdx;
	RGX_HWPERF_HOST_UFO_DATA *psData = (RGX_HWPERF_HOST_UFO_DATA *)
	        (pui8Dest + sizeof(RGX_HWPERF_V2_PACKET_HDR));
	RGX_HWPERF_UFO_DATA_ELEMENT *puData = (RGX_HWPERF_UFO_DATA_ELEMENT *)
	         psData->aui32StreamData;

	psData->eEvType = eUfoType;
	psData->ui32StreamInfo = RGX_HWPERF_MAKE_UFOPKTINFO(uiNoOfUFOs,
	        offsetof(RGX_HWPERF_HOST_UFO_DATA, aui32StreamData));

	switch (eUfoType)
	{
		case RGX_HWPERF_UFO_EV_CHECK_SUCCESS:
		case RGX_HWPERF_UFO_EV_PRCHECK_SUCCESS:
			for (uiUFOIdx = 0; uiUFOIdx < uiNoOfUFOs; uiUFOIdx++)
			{
				puData->sCheckSuccess.ui32FWAddr =
				        psUFOData[uiUFOIdx].sCheckSuccess.ui32FWAddr;
				puData->sCheckSuccess.ui32Value =
				        psUFOData[uiUFOIdx].sCheckSuccess.ui32Value;

				puData = (RGX_HWPERF_UFO_DATA_ELEMENT *)
				        (((IMG_BYTE *) puData) + sizeof(puData->sCheckSuccess));
			}
			break;
		case RGX_HWPERF_UFO_EV_CHECK_FAIL:
		case RGX_HWPERF_UFO_EV_PRCHECK_FAIL:
			for (uiUFOIdx = 0; uiUFOIdx < uiNoOfUFOs; uiUFOIdx++)
			{
				puData->sCheckFail.ui32FWAddr =
				        psUFOData[uiUFOIdx].sCheckFail.ui32FWAddr;
				puData->sCheckFail.ui32Value =
				        psUFOData[uiUFOIdx].sCheckFail.ui32Value;
				puData->sCheckFail.ui32Required =
				        psUFOData[uiUFOIdx].sCheckFail.ui32Required;

				puData = (RGX_HWPERF_UFO_DATA_ELEMENT *)
				        (((IMG_BYTE *) puData) + sizeof(puData->sCheckFail));
			}
			break;
		case RGX_HWPERF_UFO_EV_UPDATE:
			for (uiUFOIdx = 0; uiUFOIdx < uiNoOfUFOs; uiUFOIdx++)
			{
				puData->sUpdate.ui32FWAddr =
				        psUFOData[uiUFOIdx].sUpdate.ui32FWAddr;
				puData->sUpdate.ui32OldValue =
				        psUFOData[uiUFOIdx].sUpdate.ui32OldValue;
				puData->sUpdate.ui32NewValue =
				        psUFOData[uiUFOIdx].sUpdate.ui32NewValue;

				puData = (RGX_HWPERF_UFO_DATA_ELEMENT *)
				        (((IMG_BYTE *) puData) + sizeof(puData->sUpdate));
			}
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR, "RGXHWPerfHostPostUfoEvent: Invalid UFO"
			         " event type"));
			PVR_ASSERT(IMG_FALSE);
			break;
	}
}

void RGXHWPerfHostPostUfoEvent(PVRSRV_RGXDEV_INFO *psRgxDevInfo,
                               RGX_HWPERF_UFO_EV eUfoType,
                               RGX_HWPERF_UFO_DATA_ELEMENT psUFOData[],
                               IMG_UINT uiNoOfUFOs)
{
	IMG_UINT8 *pui8Dest;
	IMG_UINT32 ui32Size = _CalculateHostUfoPacketSize(eUfoType, uiNoOfUFOs);

	_PostFunctionPrologue(psRgxDevInfo);

	if ((pui8Dest = _ReserveHWPerfStream(psRgxDevInfo, ui32Size)) == NULL)
	{
		goto cleanup;
	}

	_SetupHostPacketHeader(psRgxDevInfo, pui8Dest, RGX_HWPERF_HOST_UFO, ui32Size);
	_SetupHostUfoPacketData(pui8Dest, eUfoType, psUFOData, uiNoOfUFOs);

	_CommitHWPerfStream(psRgxDevInfo, ui32Size);

cleanup:
	_PostFunctionEpilogue(psRgxDevInfo);
}

#define UNKNOWN_SYNC_NAME "UnknownSync"

static inline IMG_UINT32 _FixNameAndCalculateHostAllocPacketSize(
                                       RGX_HWPERF_HOST_RESOURCE_TYPE eAllocType,
                                       const IMG_CHAR **ppsName,
                                       IMG_UINT32 *ui32NameSize)
{
	RGX_HWPERF_HOST_ALLOC_DATA *psData;
	RGX_HWPERF_HOST_ALLOC_DETAIL *puData;
	IMG_UINT32 ui32Size = sizeof(psData->ui32AllocType);

	if (*ppsName != NULL && *ui32NameSize > 0)
	{
		/* first strip the terminator */
		if ((*ppsName)[*ui32NameSize - 1] == '\0')
			*ui32NameSize -= 1;
		/* if string longer than maximum cut it (leave space for '\0') */
		if (*ui32NameSize >= SYNC_MAX_CLASS_NAME_LEN)
			*ui32NameSize = SYNC_MAX_CLASS_NAME_LEN - 1;
	}
	else
	{
		PVR_DPF((PVR_DBG_WARNING, "RGXHWPerfHostPostAllocEvent: Invalid"
		        " resource name given."));
		*ppsName = UNKNOWN_SYNC_NAME;
		*ui32NameSize = sizeof(UNKNOWN_SYNC_NAME) - 1;
	}

	switch (eAllocType)
	{
		case RGX_HWPERF_HOST_RESOURCE_TYPE_SYNC:
			ui32Size += sizeof(puData->sSyncAlloc) - SYNC_MAX_CLASS_NAME_LEN +
			        *ui32NameSize + 1; /* +1 for '\0' */
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_TIMELINE:
			ui32Size += sizeof(puData->sTimelineAlloc) - SYNC_MAX_CLASS_NAME_LEN +
			        *ui32NameSize + 1; /* +1 for '\0' */
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_FENCE_PVR:
			ui32Size += sizeof(puData->sFenceAlloc) - SYNC_MAX_CLASS_NAME_LEN +
			        *ui32NameSize + 1; /* +1 for '\0' */
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR,
			         "RGXHWPerfHostPostAllocEvent: Invalid alloc event type"));
			PVR_ASSERT(IMG_FALSE);
			break;
	}

	return RGX_HWPERF_MAKE_SIZE_VARIABLE(ui32Size);
}

static inline void _SetupHostAllocPacketData(IMG_UINT8 *pui8Dest,
                                       RGX_HWPERF_HOST_RESOURCE_TYPE eAllocType,
                                       IMG_UINT32 ui32UID,
                                       IMG_UINT32 ui32PID,
                                       IMG_UINT32 ui32FWAddr,
                                       const IMG_CHAR *psName,
                                       IMG_UINT32 ui32NameSize)
{
	RGX_HWPERF_HOST_ALLOC_DATA *psData = (RGX_HWPERF_HOST_ALLOC_DATA *)
	        (pui8Dest + sizeof(RGX_HWPERF_V2_PACKET_HDR));

	IMG_CHAR *acName = NULL;

	psData->ui32AllocType = eAllocType;

	switch (eAllocType)
	{
		case RGX_HWPERF_HOST_RESOURCE_TYPE_SYNC:
			psData->uAllocDetail.sSyncAlloc.ui32FWAddr = ui32FWAddr;
			acName = psData->uAllocDetail.sSyncAlloc.acName;
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_TIMELINE:
			psData->uAllocDetail.sTimelineAlloc.ui32Timeline_UID1 = ui32UID;
			psData->uAllocDetail.sTimelineAlloc.uiPid = ui32PID;
			acName = psData->uAllocDetail.sTimelineAlloc.acName;
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_FENCE_PVR:
			psData->uAllocDetail.sFenceAlloc.ui32Fence_UID = ui32UID;
			psData->uAllocDetail.sFenceAlloc.ui32CheckPt_FWAddr = ui32FWAddr;
			acName = psData->uAllocDetail.sFenceAlloc.acName;
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_SYNCCP:
			psData->uAllocDetail.sSyncCheckPointAlloc.ui32Timeline_UID = ui32UID;
			psData->uAllocDetail.sSyncCheckPointAlloc.ui32CheckPt_FWAddr = ui32FWAddr;
			acName = psData->uAllocDetail.sSyncCheckPointAlloc.acName;
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR,
			         "RGXHWPerfHostPostAllocEvent: Invalid alloc event type"));
			PVR_ASSERT(IMG_FALSE);
	}


	if (ui32NameSize)
	{
		OSStringNCopy(acName, psName, ui32NameSize);
		/* we know here that string is not null terminated and that we have
		 *enough space for the terminator */
		acName[ui32NameSize] = '\0';
	}
	else
	{
		/* In case no name was given make sure we don't access random memory */
		acName[0] = '\0';
	}
}

void RGXHWPerfHostPostAllocEvent(PVRSRV_RGXDEV_INFO* psRgxDevInfo,
                                 RGX_HWPERF_HOST_RESOURCE_TYPE eAllocType,
                                 IMG_UINT32 ui32UID,
                                 IMG_UINT32 ui32PID,
                                 IMG_UINT32 ui32FWAddr,
                                 const IMG_CHAR *psName,
                                 IMG_UINT32 ui32NameSize)
{
	IMG_UINT8 *pui8Dest;
	IMG_UINT32 ui32Size = _FixNameAndCalculateHostAllocPacketSize(eAllocType,
	                                                             &psName,
	                                                             &ui32NameSize);

	_PostFunctionPrologue(psRgxDevInfo);

	if ((pui8Dest = _ReserveHWPerfStream(psRgxDevInfo, ui32Size)) == NULL)
	{
		goto cleanup;
	}

	_SetupHostPacketHeader(psRgxDevInfo, pui8Dest, RGX_HWPERF_HOST_ALLOC, ui32Size);

	_SetupHostAllocPacketData(pui8Dest,
	                          eAllocType,
	                          ui32UID,
	                          ui32PID,
	                          ui32FWAddr,
	                          psName,
	                          ui32NameSize);

	_CommitHWPerfStream(psRgxDevInfo, ui32Size);

cleanup:
	_PostFunctionEpilogue(psRgxDevInfo);
}

static inline void _SetupHostFreePacketData(IMG_UINT8 *pui8Dest,
                                          RGX_HWPERF_HOST_RESOURCE_TYPE eFreeType,
                                          IMG_UINT32 ui32UID,
                                          IMG_UINT32 ui32PID,
                                          IMG_UINT32 ui32FWAddr)
{
	RGX_HWPERF_HOST_FREE_DATA *psData = (RGX_HWPERF_HOST_FREE_DATA *)
	        (pui8Dest + sizeof(RGX_HWPERF_V2_PACKET_HDR));

	psData->ui32FreeType = eFreeType;

	switch (eFreeType)
	{
		case RGX_HWPERF_HOST_RESOURCE_TYPE_SYNC:
			psData->uFreeDetail.sSyncFree.ui32FWAddr = ui32FWAddr;
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_TIMELINE:
			psData->uFreeDetail.sTimelineDestroy.ui32Timeline_UID1 = ui32UID;
			psData->uFreeDetail.sTimelineDestroy.uiPid = ui32PID;
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_FENCE_PVR:
			psData->uFreeDetail.sFenceDestroy.ui32Fence_UID = ui32UID;
			break;
		case RGX_HWPERF_HOST_RESOURCE_TYPE_SYNCCP:
			psData->uFreeDetail.sSyncCheckPointFree.ui32CheckPt_FWAddr = ui32FWAddr;
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR,
			         "RGXHWPerfHostPostFreeEvent: Invalid free event type"));
			PVR_ASSERT(IMG_FALSE);
	}
}

void RGXHWPerfHostPostFreeEvent(PVRSRV_RGXDEV_INFO *psRgxDevInfo,
                                RGX_HWPERF_HOST_RESOURCE_TYPE eFreeType,
                                IMG_UINT32 ui32UID,
                                IMG_UINT32 ui32PID,
                                IMG_UINT32 ui32FWAddr)
{
	IMG_UINT8 *pui8Dest;
	IMG_UINT32 ui32Size = RGX_HWPERF_MAKE_SIZE_FIXED(RGX_HWPERF_HOST_FREE_DATA);

	_PostFunctionPrologue(psRgxDevInfo);

	if ((pui8Dest = _ReserveHWPerfStream(psRgxDevInfo, ui32Size)) == NULL)
	{
		goto cleanup;
	}

	_SetupHostPacketHeader(psRgxDevInfo, pui8Dest, RGX_HWPERF_HOST_FREE, ui32Size);
	_SetupHostFreePacketData(pui8Dest,
	                         eFreeType,
                             ui32UID,
                             ui32PID,
                             ui32FWAddr);

	_CommitHWPerfStream(psRgxDevInfo, ui32Size);

cleanup:
	_PostFunctionEpilogue(psRgxDevInfo);
}

static inline IMG_UINT32 _FixNameAndCalculateHostModifyPacketSize(
                                      RGX_HWPERF_HOST_RESOURCE_TYPE eModifyType,
                                      const IMG_CHAR **ppsName,
                                      IMG_UINT32 *ui32NameSize)
{
	RGX_HWPERF_HOST_MODIFY_DATA *psData;
	RGX_HWPERF_HOST_MODIFY_DETAIL *puData;
	IMG_UINT32 ui32Size = sizeof(psData->ui32ModifyType);

	if (*ppsName != NULL && *ui32NameSize > 0)
	{
		/* first strip the terminator */
		if ((*ppsName)[*ui32NameSize - 1] == '\0')
			*ui32NameSize -= 1;
		/* if string longer than maximum cut it (leave space for '\0') */
		if (*ui32NameSize >= SYNC_MAX_CLASS_NAME_LEN)
			*ui32NameSize = SYNC_MAX_CLASS_NAME_LEN - 1;
	}
	else
	{
		PVR_DPF((PVR_DBG_WARNING, "RGXHWPerfHostPostModifyEvent: Invalid"
		        " resource name given."));
		*ppsName = UNKNOWN_SYNC_NAME;
		*ui32NameSize = sizeof(UNKNOWN_SYNC_NAME) - 1;
	}

	switch (eModifyType)
	{
		case RGX_HWPERF_HOST_RESOURCE_TYPE_FENCE_PVR:
			ui32Size += sizeof(puData->sFenceMerge) - SYNC_MAX_CLASS_NAME_LEN +
			        *ui32NameSize + 1; /* +1 for '\0' */
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR,
			         "RGXHWPerfHostPostModifyEvent: Invalid modify event type"));
			PVR_ASSERT(IMG_FALSE);
			break;
	}

	return RGX_HWPERF_MAKE_SIZE_VARIABLE(ui32Size);
}

static inline void _SetupHostModifyPacketData(IMG_UINT8 *pui8Dest,
                                              RGX_HWPERF_HOST_RESOURCE_TYPE eModifyType,
                                              IMG_UINT32 ui32NewUID,
                                              IMG_UINT32 ui32UID1,
                                              IMG_UINT32 ui32UID2,
                                              const IMG_CHAR *psName,
                                              IMG_UINT32 ui32NameSize)
{
	RGX_HWPERF_HOST_MODIFY_DATA *psData = (RGX_HWPERF_HOST_MODIFY_DATA *)
	        (pui8Dest + sizeof(RGX_HWPERF_V2_PACKET_HDR));

	IMG_CHAR *acName = NULL;

	psData->ui32ModifyType = eModifyType;

	switch (eModifyType)
	{
		case RGX_HWPERF_HOST_RESOURCE_TYPE_FENCE_PVR:
			psData->uModifyDetail.sFenceMerge.ui32NewFence_UID = ui32NewUID;
			psData->uModifyDetail.sFenceMerge.ui32InFence1_UID = ui32UID1;
			psData->uModifyDetail.sFenceMerge.ui32InFence2_UID = ui32UID2;
			acName = psData->uModifyDetail.sFenceMerge.acName;
			break;
		default:
			// unknown type - this should never happen
			PVR_DPF((PVR_DBG_ERROR,
			         "RGXHWPerfHostPostModifyEvent: Invalid modify event type"));
			PVR_ASSERT(IMG_FALSE);
	}

	if (ui32NameSize)
	{
		OSStringNCopy(acName, psName, ui32NameSize);
		/* we know here that string is not null terminated and that we have
		 * enough space for the terminator */
		acName[ui32NameSize] = '\0';
	}
	else
	{
		/* In case no name was given make sure we don't access random memory */
		acName[0] = '\0';
	}
}

void RGXHWPerfHostPostModifyEvent(PVRSRV_RGXDEV_INFO *psRgxDevInfo,
                                  RGX_HWPERF_HOST_RESOURCE_TYPE eModifyType,
                                  IMG_UINT32 ui32NewUID,
                                  IMG_UINT32 ui32UID1,
                                  IMG_UINT32 ui32UID2,
                                  const IMG_CHAR *psName,
                                  IMG_UINT32 ui32NameSize)
{
	IMG_UINT8 *pui8Dest;
	IMG_UINT32 ui32Size = _FixNameAndCalculateHostModifyPacketSize(eModifyType,
			                                                     &psName,
			                                                     &ui32NameSize);

	_PostFunctionPrologue(psRgxDevInfo);

	if ((pui8Dest = _ReserveHWPerfStream(psRgxDevInfo, ui32Size)) == NULL)
	{
		goto cleanup;
	}

	_SetupHostPacketHeader(psRgxDevInfo, pui8Dest, RGX_HWPERF_HOST_MODIFY, ui32Size);
	_SetupHostModifyPacketData(pui8Dest,
	                           eModifyType,
	                           ui32NewUID,
	                           ui32UID1,
	                           ui32UID2,
	                           psName,
                               ui32NameSize);


cleanup:
	_PostFunctionEpilogue(psRgxDevInfo);
}

static inline void _SetupHostClkSyncPacketData(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_UINT8 *pui8Dest)
{
	RGX_HWPERF_HOST_CLK_SYNC_DATA *psData = (RGX_HWPERF_HOST_CLK_SYNC_DATA *)
	        (pui8Dest + sizeof(RGX_HWPERF_V2_PACKET_HDR));
	RGXFWIF_GPU_UTIL_FWCB *psGpuUtilFWCB = psRgxDevInfo->psRGXFWIfGpuUtilFWCb;
	IMG_UINT32 ui32CurrIdx =
	        RGXFWIF_TIME_CORR_CURR_INDEX(psGpuUtilFWCB->ui32TimeCorrSeqCount);
	RGXFWIF_TIME_CORR *psTimeCorr = &psGpuUtilFWCB->sTimeCorr[ui32CurrIdx];

	psData->ui64CRTimestamp = psTimeCorr->ui64CRTimeStamp;
	psData->ui64OSTimestamp = psTimeCorr->ui64OSTimeStamp;
	psData->ui32ClockSpeed = psTimeCorr->ui32CoreClockSpeed;
}

void RGXHWPerfHostPostClkSyncEvent(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	IMG_UINT8 *pui8Dest;
	IMG_UINT32 ui32Size =
	        RGX_HWPERF_MAKE_SIZE_FIXED(RGX_HWPERF_HOST_CLK_SYNC_DATA);

	_PostFunctionPrologue(psRgxDevInfo);

	if ((pui8Dest = _ReserveHWPerfStream(psRgxDevInfo, ui32Size)) == NULL)
	{
		goto cleanup;
	}

	_SetupHostPacketHeader(psRgxDevInfo, pui8Dest, RGX_HWPERF_HOST_CLK_SYNC, ui32Size);
	_SetupHostClkSyncPacketData(psRgxDevInfo, pui8Dest);

	_CommitHWPerfStream(psRgxDevInfo, ui32Size);

cleanup:
	_PostFunctionEpilogue(psRgxDevInfo);
}

/******************************************************************************
 * SUPPORT_GPUTRACE_EVENTS
 *
 * Currently only implemented on Linux and Android. Feature can be enabled on
 * Android builds but can also be enabled on Linux builds for testing
 * but requires the gpu.h FTrace event header file to be present.
 *****************************************************************************/
#if defined(SUPPORT_GPUTRACE_EVENTS)

/* Saved value of the clock source before the trace was enabled. We're keeping
 * it here so that we know which clock should be selected after we disable the
 * gpu ftrace. */
static RGXTIMECORR_CLOCK_TYPE geLastTimeCorrClock = PVRSRV_APPHINT_TIMECORRCLOCK;

/* This lock ensures that the reference counting operation on the FTrace UFO
 * events and enable/disable operation on firmware event are performed as
 * one atomic operation. This should ensure that there are no race conditions
 * between reference counting and firmware event state change.
 * See below comment for guiUfoEventRef.
 */
static POS_LOCK ghLockFTraceEventLock;

/* Multiple FTrace UFO events are reflected in the firmware as only one event. When
 * we enable FTrace UFO event we want to also at the same time enable it in
 * the firmware. Since there is a multiple-to-one relation between those events
 * we count how many FTrace UFO events is enabled. If at least one event is
 * enabled we enabled the firmware event. When all FTrace UFO events are disabled
 * we disable firmware event. */
static IMG_UINT guiUfoEventRef;

static void RGXHWPerfFTraceCmdCompleteNotify(PVRSRV_CMDCOMP_HANDLE);

typedef struct RGX_HWPERF_FTRACE_DATA {
	/* This lock ensures the HWPerf TL stream reading resources are not destroyed
	 * by one thread disabling it while another is reading from it. Keeps the
	 * state and resource create/destroy atomic and consistent. */
	POS_LOCK    hFTraceResourceLock;

	IMG_HANDLE  hGPUTraceCmdCompleteHandle;
	IMG_HANDLE  hGPUTraceTLStream;
	IMG_UINT64  ui64LastSampledTimeCorrOSTimeStamp;
	IMG_UINT32  ui32FTraceLastOrdinal;
} RGX_HWPERF_FTRACE_DATA;

/* Caller must now hold hFTraceResourceLock before calling this method.
 */
static PVRSRV_ERROR RGXHWPerfFTraceGPUEnable(PVRSRV_RGXDEV_INFO *psRgxDevInfo)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	RGX_HWPERF_FTRACE_DATA *psFtraceData;
	PVRSRV_DEVICE_NODE *psRgxDevNode = psRgxDevInfo->psDeviceNode;
	IMG_CHAR pszHWPerfStreamName[sizeof(PVRSRV_TL_HWPERF_RGX_FW_STREAM) + 5];

	PVR_DPF_ENTERED;

	PVR_ASSERT(psRgxDevInfo);

	psFtraceData = psRgxDevInfo->pvGpuFtraceData;

	PVR_ASSERT(OSLockIsLocked(psFtraceData->hFTraceResourceLock));

	/* return if already enabled */
	if (psFtraceData->hGPUTraceTLStream)
	{
		return PVRSRV_OK;
	}

	/* Signal FW to enable event generation */
	if (psRgxDevInfo->bFirmwareInitialised)
	{
		IMG_UINT64 ui64UFOFilter = psRgxDevInfo->ui64HWPerfFilter &
		        (RGX_HWPERF_EVENT_MASK_FW_SED | RGX_HWPERF_EVENT_MASK_FW_UFO);

		eError = PVRSRVRGXCtrlHWPerfKM(NULL, psRgxDevNode,
		                               RGX_HWPERF_STREAM_ID0_FW, IMG_FALSE,
		                               RGX_HWPERF_EVENT_MASK_HW_KICKFINISH |
		                               ui64UFOFilter);
		PVR_LOGG_IF_ERROR(eError, "PVRSRVRGXCtrlHWPerfKM", err_out);
	}
	else
	{
		/* only set filter and exit */
		psRgxDevInfo->ui64HWPerfFilter = RGX_HWPERF_EVENT_MASK_HW_KICKFINISH |
		        ((RGX_HWPERF_EVENT_MASK_FW_SED | RGX_HWPERF_EVENT_MASK_FW_UFO) &
		        psRgxDevInfo->ui64HWPerfFilter);

		PVR_DPF((PVR_DBG_WARNING, "HWPerfFW mask has been SET to (%llx)",
		        (long long) psRgxDevInfo->ui64HWPerfFilter));

		return PVRSRV_OK;
	}

	/* form the HWPerf stream name, corresponding to this DevNode; which can make sense in the UM */
	if (OSSNPrintf(pszHWPerfStreamName, sizeof(pszHWPerfStreamName), "%s%d",
					PVRSRV_TL_HWPERF_RGX_FW_STREAM, psRgxDevNode->sDevId.i32UMIdentifier) < 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to form HWPerf stream name for device %d",
		                        __FUNCTION__,
								psRgxDevNode->sDevId.i32UMIdentifier));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* Open the TL Stream for HWPerf data consumption */
	eError = TLClientOpenStream(DIRECT_BRIDGE_HANDLE,
								pszHWPerfStreamName,
								PVRSRV_STREAM_FLAG_ACQUIRE_NONBLOCKING,
								&psFtraceData->hGPUTraceTLStream);
	PVR_LOGG_IF_ERROR(eError, "TLClientOpenStream", err_out);

	if (RGXGPUFreqCalibrateGetClockSource() != RGXTIMECORR_CLOCK_SCHED)
	{
		/* Set clock source for timer correlation data to sched_clock */
		geLastTimeCorrClock = RGXGPUFreqCalibrateGetClockSource();
		RGXGPUFreqCalibrateSetClockSource(psRgxDevNode, RGXTIMECORR_CLOCK_SCHED);
	}

	/* Reset the OS timestamp coming from the timer correlation data
	 * associated with the latest HWPerf event we processed.
	 */
	psFtraceData->ui64LastSampledTimeCorrOSTimeStamp = 0;

	/* Register a notifier to collect HWPerf data whenever the HW completes
	 * an operation.
	 */
	eError = PVRSRVRegisterCmdCompleteNotify(
		&psFtraceData->hGPUTraceCmdCompleteHandle,
		&RGXHWPerfFTraceCmdCompleteNotify,
		psRgxDevInfo);
	PVR_LOGG_IF_ERROR(eError, "PVRSRVRegisterCmdCompleteNotify", err_close_stream);

err_out:
	PVR_DPF_RETURN_RC(eError);

err_close_stream:
	TLClientCloseStream(DIRECT_BRIDGE_HANDLE,
						psFtraceData->hGPUTraceTLStream);
	psFtraceData->hGPUTraceTLStream = NULL;
	goto err_out;
}

/* Caller must now hold hFTraceResourceLock before calling this method.
 */
static PVRSRV_ERROR RGXHWPerfFTraceGPUDisable(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_BOOL bDeInit)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	RGX_HWPERF_FTRACE_DATA *psFtraceData;
	PVRSRV_DEVICE_NODE *psRgxDevNode = psRgxDevInfo->psDeviceNode;

	PVR_DPF_ENTERED;

	PVR_ASSERT(psRgxDevInfo);

	psFtraceData = psRgxDevInfo->pvGpuFtraceData;

	PVR_ASSERT(OSLockIsLocked(psFtraceData->hFTraceResourceLock));

	/* if FW is not yet initialised, just set filter and exit */
	if (!psRgxDevInfo->bFirmwareInitialised)
	{
		psRgxDevInfo->ui64HWPerfFilter = RGX_HWPERF_EVENT_MASK_NONE;
		PVR_DPF((PVR_DBG_WARNING, "HWPerfFW mask has been SET to (%llx)",
		        (long long) psRgxDevInfo->ui64HWPerfFilter));

		return PVRSRV_OK;
	}

	if (NULL == psFtraceData->hGPUTraceTLStream)
	{
		/* Tracing already disabled, just return */
		return PVRSRV_OK;
	}

	if (!bDeInit)
	{
		eError = PVRSRVRGXCtrlHWPerfKM(NULL, psRgxDevNode,
		                               RGX_HWPERF_STREAM_ID0_FW, IMG_FALSE,
		                               (RGX_HWPERF_EVENT_MASK_NONE));
		PVR_LOG_IF_ERROR(eError, "PVRSRVRGXCtrlHWPerfKM");
	}

	if (psFtraceData->hGPUTraceCmdCompleteHandle)
	{
		/* Tracing is being turned off. Unregister the notifier. */
		eError = PVRSRVUnregisterCmdCompleteNotify(
				psFtraceData->hGPUTraceCmdCompleteHandle);
		PVR_LOG_IF_ERROR(eError, "PVRSRVUnregisterCmdCompleteNotify");
		psFtraceData->hGPUTraceCmdCompleteHandle = NULL;
	}

	if (psFtraceData->hGPUTraceTLStream)
	{
		IMG_PBYTE pbTmp = NULL;
		IMG_UINT32 ui32Tmp = 0;

		/* We have to flush both the L1 (FW) and L2 (Host) buffers in case there
		 * are some events left unprocessed in this FTrace/systrace "session"
		 * (note that even if we have just disabled HWPerf on the FW some packets
		 * could have been generated and already copied to L2 by the MISR handler).
		 *
		 * With the following calls we will both copy new data to the Host buffer
		 * (done by the producer callback in TLClientAcquireData) and advance
		 * the read offset in the buffer to catch up with the latest events.
		 */
		eError = TLClientAcquireData(DIRECT_BRIDGE_HANDLE,
		                             psFtraceData->hGPUTraceTLStream,
		                             &pbTmp, &ui32Tmp);
		PVR_LOG_IF_ERROR(eError, "TLClientCloseStream");

		/* Let close stream perform the release data on the outstanding acquired data */
		eError = TLClientCloseStream(DIRECT_BRIDGE_HANDLE,
		                             psFtraceData->hGPUTraceTLStream);
		PVR_LOG_IF_ERROR(eError, "TLClientCloseStream");

		psFtraceData->hGPUTraceTLStream = NULL;
	}

	if (geLastTimeCorrClock != RGXTIMECORR_CLOCK_SCHED)
	{
		RGXGPUFreqCalibrateSetClockSource(psRgxDevNode, geLastTimeCorrClock);
	}

	PVR_DPF_RETURN_RC(eError);
}

PVRSRV_ERROR RGXHWPerfFTraceGPUEventsEnabledSet(PVRSRV_RGXDEV_INFO *psRgxDevInfo, IMG_BOOL bNewValue)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	RGX_HWPERF_FTRACE_DATA *psFtraceData;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	PVR_DPF_ENTERED;

	PVR_ASSERT(psRgxDevInfo);
	psFtraceData = psRgxDevInfo->pvGpuFtraceData;

	/* About to create/destroy FTrace resources, lock critical section
	 * to avoid HWPerf MISR thread contention.
	 */
	OSLockAcquire(psFtraceData->hFTraceResourceLock);

	eError = (bNewValue ? RGXHWPerfFTraceGPUEnable(psRgxDevInfo)
					   : RGXHWPerfFTraceGPUDisable(psRgxDevInfo, IMG_FALSE));

	OSLockRelease(psFtraceData->hFTraceResourceLock);

	PVR_DPF_RETURN_RC(eError);
}

PVRSRV_ERROR PVRGpuTraceEnabledSet(IMG_BOOL bNewValue)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PVRSRV_DEVICE_NODE *psDeviceNode;

	/* This entry point from DebugFS must take the global
	 * bridge lock at this outer level of the stack before calling
	 * into the RGX part of the driver which can lead to RGX
	 * device data changes and communication with the FW which
	 * all requires the bridge lock.
	 */
#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSAcquireBridgeLock();
#endif
	psDeviceNode = psPVRSRVData->psDeviceNodeList;
	/* enable/disable GPU trace on all devices */
	while (psDeviceNode)
	{
		eError = RGXHWPerfFTraceGPUEventsEnabledSet(psDeviceNode->pvDevice, bNewValue);
		if (eError != PVRSRV_OK)
		{
			break;
		}
		psDeviceNode = psDeviceNode->psNext;
	}
#if defined(PVRSRV_USE_BRIDGE_LOCK)
	OSReleaseBridgeLock();
#endif

	PVR_DPF_RETURN_RC(eError);
}

PVRSRV_ERROR PVRGpuTraceEnabledSetNoBridgeLock(PVRSRV_DEVICE_NODE *psDeviceNode,
                                               IMG_BOOL bNewValue)
{
	return RGXHWPerfFTraceGPUEventsEnabledSet(psDeviceNode->pvDevice, bNewValue);
}

/* Calculate the OS timestamp given an RGX timestamp in the HWPerf event. */
static uint64_t
CalculateEventTimestamp(PVRSRV_RGXDEV_INFO *psDevInfo,
						uint32_t ui32TimeCorrIndex,
						uint64_t ui64EventTimestamp)
{
	RGXFWIF_GPU_UTIL_FWCB *psGpuUtilFWCB = psDevInfo->psRGXFWIfGpuUtilFWCb;
	RGX_HWPERF_FTRACE_DATA *psFtraceData = psDevInfo->pvGpuFtraceData;
	RGXFWIF_TIME_CORR *psTimeCorr = &psGpuUtilFWCB->sTimeCorr[ui32TimeCorrIndex];
	uint64_t ui64CRTimeStamp = psTimeCorr->ui64CRTimeStamp;
	uint64_t ui64OSTimeStamp = psTimeCorr->ui64OSTimeStamp;
	uint64_t ui64CRDeltaToOSDeltaKNs = psTimeCorr->ui64CRDeltaToOSDeltaKNs;
	uint64_t ui64EventOSTimestamp, deltaRgxTimer, delta_ns;

	if (psFtraceData->ui64LastSampledTimeCorrOSTimeStamp > ui64OSTimeStamp)
	{
		/* The previous packet had a time reference (time correlation data) more
		 * recent than the one in the current packet, it means the timer
		 * correlation array wrapped too quickly (buffer too small) and in the
		 * previous call to RGXHWPerfFTraceGPUUfoEvent we read one of the
		 * newest timer correlations rather than one of the oldest ones.
		 */
		PVR_DPF((PVR_DBG_ERROR, "%s: The timestamps computed so far could be "
				 "wrong! The time correlation array size should be increased "
				 "to avoid this.", __func__));
	}

	psFtraceData->ui64LastSampledTimeCorrOSTimeStamp = ui64OSTimeStamp;

	/* RGX CR timer ticks delta */
	deltaRgxTimer = ui64EventTimestamp - ui64CRTimeStamp;
	/* RGX time delta in nanoseconds */
	delta_ns = RGXFWIF_GET_DELTA_OSTIME_NS(deltaRgxTimer, ui64CRDeltaToOSDeltaKNs);
	/* Calculate OS time of HWPerf event */
	ui64EventOSTimestamp = ui64OSTimeStamp + delta_ns;

	PVR_DPF((PVR_DBG_VERBOSE, "%s: psCurrentDvfs RGX %llu, OS %llu, DVFSCLK %u",
			 __func__, ui64CRTimeStamp, ui64OSTimeStamp,
			 psTimeCorr->ui32CoreClockSpeed));

	return ui64EventOSTimestamp;
}

void RGXHWPerfFTraceGPUEnqueueEvent(PVRSRV_RGXDEV_INFO *psDevInfo,
		IMG_UINT32 ui32CtxId, IMG_UINT32 ui32JobId,
		RGX_HWPERF_KICK_TYPE eKickType)
{
	PVRSRV_VZ_RETN_IF_MODE(DRIVER_MODE_GUEST);

	PVR_DPF_ENTERED;

	PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfFTraceGPUEnqueueEvent: ui32CtxId %u, "
	        "ui32JobId %u", ui32CtxId, ui32JobId));

	PVRGpuTraceClientWork(psDevInfo->psDeviceNode, ui32CtxId, ui32JobId,
	    RGXHWPerfKickTypeToStr(eKickType));

	PVR_DPF_RETURN;
}


static void RGXHWPerfFTraceGPUSwitchEvent(PVRSRV_RGXDEV_INFO *psDevInfo,
		RGX_HWPERF_V2_PACKET_HDR* psHWPerfPkt, const IMG_CHAR* pszWorkName,
		PVR_GPUTRACE_SWITCH_TYPE eSwType)
{
	IMG_UINT64 ui64Timestamp;
	RGX_HWPERF_HW_DATA* psHWPerfPktData;

	PVR_DPF_ENTERED;

	PVR_ASSERT(psHWPerfPkt);
	PVR_ASSERT(pszWorkName);

	psHWPerfPktData = (RGX_HWPERF_HW_DATA*) RGX_HWPERF_GET_PACKET_DATA_BYTES(psHWPerfPkt);

	ui64Timestamp = CalculateEventTimestamp(psDevInfo, psHWPerfPktData->ui32TimeCorrIndex,
											psHWPerfPkt->ui64Timestamp);

	PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfFTraceGPUSwitchEvent: %s ui32ExtJobRef=%d, ui32IntJobRef=%d, eSwType=%d",
			pszWorkName, psHWPerfPktData->ui32DMContext, psHWPerfPktData->ui32IntJobRef, eSwType));

	PVRGpuTraceWorkSwitch(ui64Timestamp, psHWPerfPktData->ui32DMContext, psHWPerfPktData->ui32CtxPriority,
	                      psHWPerfPktData->ui32IntJobRef, pszWorkName, eSwType);

	PVR_DPF_RETURN;
}

static void RGXHWPerfFTraceGPUUfoEvent(PVRSRV_RGXDEV_INFO *psDevInfo,
                                       RGX_HWPERF_V2_PACKET_HDR* psHWPerfPkt)
{
	IMG_UINT64 ui64Timestamp;
	RGX_HWPERF_UFO_DATA *psHWPerfPktData;
	IMG_UINT32 ui32UFOCount;
	RGX_HWPERF_UFO_DATA_ELEMENT *puData;

	psHWPerfPktData = (RGX_HWPERF_UFO_DATA *)
	        RGX_HWPERF_GET_PACKET_DATA_BYTES(psHWPerfPkt);

	ui32UFOCount = RGX_HWPERF_GET_UFO_STREAMSIZE(psHWPerfPktData->ui32StreamInfo);
	puData = (RGX_HWPERF_UFO_DATA_ELEMENT *) (((IMG_BYTE *) psHWPerfPktData)
	        + RGX_HWPERF_GET_UFO_STREAMOFFSET(psHWPerfPktData->ui32StreamInfo));

	ui64Timestamp = CalculateEventTimestamp(psDevInfo, psHWPerfPktData->ui32TimeCorrIndex,
											psHWPerfPkt->ui64Timestamp);

	PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfFTraceGPUUfoEvent: ui32ExtJobRef=%d, "
	        "ui32IntJobRef=%d", psHWPerfPktData->ui32ExtJobRef,
	        psHWPerfPktData->ui32IntJobRef));

	PVRGpuTraceUfo(ui64Timestamp, psHWPerfPktData->eEvType,
	        psHWPerfPktData->ui32ExtJobRef, psHWPerfPktData->ui32DMContext,
	        psHWPerfPktData->ui32IntJobRef, ui32UFOCount, puData);
}

static void RGXHWPerfFTraceGPUFirmwareEvent(PVRSRV_RGXDEV_INFO *psDevInfo,
		RGX_HWPERF_V2_PACKET_HDR* psHWPerfPkt, const IMG_CHAR* pszWorkName,
		PVR_GPUTRACE_SWITCH_TYPE eSwType)

{
	uint64_t ui64Timestamp;
	RGX_HWPERF_FW_DATA *psHWPerfPktData = (RGX_HWPERF_FW_DATA *)
		RGX_HWPERF_GET_PACKET_DATA_BYTES(psHWPerfPkt);

	ui64Timestamp = CalculateEventTimestamp(psDevInfo, psHWPerfPktData->ui32TimeCorrIndex,
											psHWPerfPkt->ui64Timestamp);

	PVRGpuTraceFirmware(ui64Timestamp, pszWorkName, eSwType);
}

static IMG_BOOL ValidAndEmitFTraceEvent(PVRSRV_RGXDEV_INFO *psDevInfo,
		RGX_HWPERF_V2_PACKET_HDR* psHWPerfPkt)
{
	RGX_HWPERF_EVENT_TYPE eType;
	RGX_HWPERF_FTRACE_DATA *psFtraceData = psDevInfo->pvGpuFtraceData;
	IMG_UINT32 ui32HwEventTypeIndex;
	static const struct {
		IMG_CHAR* pszName;
		PVR_GPUTRACE_SWITCH_TYPE eSwType;
	} aszHwEventTypeMap[] = {
			{ /* RGX_HWPERF_FW_BGSTART */      "BG",     PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_FW_BGEND */        "BG",     PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_FW_IRQSTART */     "IRQ",     PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_FW_IRQEND */       "IRQ",     PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_FW_DBGSTART */     "DBG",     PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_FW_DBGEND */       "DBG",     PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_PMOOM_TAPAUSE */	"PMOOM_TAPAUSE",  PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_TAKICK */       "TA",     PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_TAFINISHED */   "TA",     PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_3DTQKICK */     "TQ3D",   PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_3DKICK */       "3D",     PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_3DFINISHED */   "3D",     PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_CDMKICK */      "CDM",    PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_CDMFINISHED */  "CDM",    PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_TLAKICK */      "TQ2D",   PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_TLAFINISHED */  "TQ2D",   PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_3DSPMKICK */    "3DSPM",  PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_PERIODIC */     NULL, 0 }, /* PERIODIC not supported */
			{ /* RGX_HWPERF_HW_RTUKICK */      "RTU",    PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_RTUFINISHED */  "RTU",    PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_SHGKICK */      "SHG",    PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_SHGFINISHED */  "SHG",    PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_3DTQFINISHED */ "TQ3D",   PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_3DSPMFINISHED */ "3DSPM", PVR_GPUTRACE_SWITCH_TYPE_END },
			{ /* RGX_HWPERF_HW_PMOOM_TARESUME */	"PMOOM_TARESUME",  PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_TDMKICK */      "TDM",   PVR_GPUTRACE_SWITCH_TYPE_BEGIN },
			{ /* RGX_HWPERF_HW_TDMFINISHED */  "TDM",   PVR_GPUTRACE_SWITCH_TYPE_END },
	};
	static_assert(RGX_HWPERF_HW_EVENT_RANGE0_FIRST_TYPE == RGX_HWPERF_FW_EVENT_RANGE_LAST_TYPE + 1,
				  "FW and HW events are not contiguous in RGX_HWPERF_EVENT_TYPE");

	PVR_ASSERT(psHWPerfPkt);
	eType = RGX_HWPERF_GET_TYPE(psHWPerfPkt);

	if (psFtraceData->ui32FTraceLastOrdinal != psHWPerfPkt->ui32Ordinal - 1)
	{
		RGX_HWPERF_STREAM_ID eStreamId = RGX_HWPERF_GET_STREAM_ID(psHWPerfPkt);
		PVRGpuTraceEventsLost(eStreamId,
		                      psFtraceData->ui32FTraceLastOrdinal,
		                      psHWPerfPkt->ui32Ordinal);
		PVR_DPF((PVR_DBG_ERROR, "FTrace events lost (stream_id = %u, ordinal: last = %u, current = %u)",
		         eStreamId, psFtraceData->ui32FTraceLastOrdinal, psHWPerfPkt->ui32Ordinal));
	}

	psFtraceData->ui32FTraceLastOrdinal = psHWPerfPkt->ui32Ordinal;

	/* Process UFO packets */
	if (eType == RGX_HWPERF_UFO)
	{
		RGXHWPerfFTraceGPUUfoEvent(psDevInfo, psHWPerfPkt);
		return IMG_TRUE;
	}

	if (eType <= RGX_HWPERF_HW_EVENT_RANGE0_LAST_TYPE)
	{
		/* this ID belongs to range 0, so index directly in range 0 */
		ui32HwEventTypeIndex = eType - RGX_HWPERF_FW_EVENT_RANGE_FIRST_TYPE;
	}
	else
	{
		/* this ID belongs to range 1, so first index in range 1 and skip number of slots used up for range 0 */
		ui32HwEventTypeIndex = (eType - RGX_HWPERF_HW_EVENT_RANGE1_FIRST_TYPE) +
		                       (RGX_HWPERF_HW_EVENT_RANGE0_LAST_TYPE - RGX_HWPERF_FW_EVENT_RANGE_FIRST_TYPE + 1);
	}

	if (ui32HwEventTypeIndex >= IMG_ARR_NUM_ELEMS(aszHwEventTypeMap))
		goto err_unsupported;

	if (aszHwEventTypeMap[ui32HwEventTypeIndex].pszName == NULL)
	{
		/* Not supported map entry, ignore event */
		goto err_unsupported;
	}

	if (HWPERF_PACKET_IS_HW_TYPE(eType))
	{
		RGXHWPerfFTraceGPUSwitchEvent(psDevInfo, psHWPerfPkt,
									  aszHwEventTypeMap[ui32HwEventTypeIndex].pszName,
									  aszHwEventTypeMap[ui32HwEventTypeIndex].eSwType);
	}
	else if (HWPERF_PACKET_IS_FW_TYPE(eType))
	{
		RGXHWPerfFTraceGPUFirmwareEvent(psDevInfo, psHWPerfPkt,
										aszHwEventTypeMap[ui32HwEventTypeIndex].pszName,
										aszHwEventTypeMap[ui32HwEventTypeIndex].eSwType);
	}
	else
	{
		goto err_unsupported;
	}

	return IMG_TRUE;

err_unsupported:
	PVR_DPF((PVR_DBG_VERBOSE, "%s: Unsupported event type %d", __func__, eType));
	return IMG_FALSE;
}


static void RGXHWPerfFTraceGPUProcessPackets(PVRSRV_RGXDEV_INFO *psDevInfo,
		IMG_PBYTE pBuffer, IMG_UINT32 ui32ReadLen)
{
	IMG_UINT32			ui32TlPackets = 0;
	IMG_UINT32			ui32HWPerfPackets = 0;
	IMG_UINT32			ui32HWPerfPacketsSent = 0;
	IMG_PBYTE			pBufferEnd;
	PVRSRVTL_PPACKETHDR psHDRptr;
	PVRSRVTL_PACKETTYPE ui16TlType;

	PVR_DPF_ENTERED;

	PVR_ASSERT(psDevInfo);
	PVR_ASSERT(pBuffer);
	PVR_ASSERT(ui32ReadLen);

	/* Process the TL Packets
	 */
	pBufferEnd = pBuffer+ui32ReadLen;
	psHDRptr = GET_PACKET_HDR(pBuffer);
	while ( psHDRptr < (PVRSRVTL_PPACKETHDR)pBufferEnd )
	{
		ui16TlType = GET_PACKET_TYPE(psHDRptr);
		if (ui16TlType == PVRSRVTL_PACKETTYPE_DATA)
		{
			IMG_UINT16 ui16DataLen = GET_PACKET_DATA_LEN(psHDRptr);
			if (0 == ui16DataLen)
			{
				PVR_DPF((PVR_DBG_ERROR, "RGXHWPerfFTraceGPUProcessPackets: ZERO Data in TL data packet: %p", psHDRptr));
			}
			else
			{
				RGX_HWPERF_V2_PACKET_HDR* psHWPerfPkt;
				RGX_HWPERF_V2_PACKET_HDR* psHWPerfEnd;

				/* Check for lost hwperf data packets */
				psHWPerfEnd = RGX_HWPERF_GET_PACKET(GET_PACKET_DATA_PTR(psHDRptr)+ui16DataLen);
				psHWPerfPkt = RGX_HWPERF_GET_PACKET(GET_PACKET_DATA_PTR(psHDRptr));
				do
				{
					if (ValidAndEmitFTraceEvent(psDevInfo, psHWPerfPkt))
					{
						ui32HWPerfPacketsSent++;
					}
					ui32HWPerfPackets++;
					psHWPerfPkt = RGX_HWPERF_GET_NEXT_PACKET(psHWPerfPkt);
				}
				while (psHWPerfPkt < psHWPerfEnd);
			}
		}
		else if (ui16TlType == PVRSRVTL_PACKETTYPE_MOST_RECENT_WRITE_FAILED)
		{
			PVR_DPF((PVR_DBG_MESSAGE, "RGXHWPerfFTraceGPUProcessPackets: Indication that the transport buffer was full"));
		}
		else
		{
			/* else Ignore padding packet type and others */
			PVR_DPF((PVR_DBG_MESSAGE, "RGXHWPerfFTraceGPUProcessPackets: Ignoring TL packet, type %d", ui16TlType ));
		}

		psHDRptr = GET_NEXT_PACKET_ADDR(psHDRptr);
		ui32TlPackets++;
	}

	PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfFTraceGPUProcessPackets: TL "
	 		"Packets processed %03d, HWPerf packets %03d, sent %03d",
	 		ui32TlPackets, ui32HWPerfPackets, ui32HWPerfPacketsSent));

	PVR_DPF_RETURN;
}


static
void RGXHWPerfFTraceCmdCompleteNotify(PVRSRV_CMDCOMP_HANDLE hCmdCompHandle)
{
	PVRSRV_RGXDEV_INFO* psDeviceInfo = hCmdCompHandle;
	RGX_HWPERF_FTRACE_DATA* psFtraceData;
	PVRSRV_ERROR		eError;
	IMG_PBYTE			pBuffer;
	IMG_UINT32			ui32ReadLen;

	PVR_DPF_ENTERED;

	/* Exit if no HWPerf enabled device exits */
	PVR_ASSERT(psDeviceInfo != NULL);

	psFtraceData = psDeviceInfo->pvGpuFtraceData;

	/* Command-complete notifiers can run concurrently. If this is
	 * happening, just bail out and let the previous call finish.
	 * This is ok because we can process the queued packets on the next call.
	 */
	if (!OSTryLockAcquire(psFtraceData->hFTraceResourceLock))
	{
		PVR_DPF_RETURN;
	}

	/* If this notifier is called, it means the TL resources will be valid at-least
	 * until the end of this call, since the DeInit function will wait on the hFTraceResourceLock
	 * to clean-up the TL resources and un-register the notifier, so just assert here.
	 */
	PVR_ASSERT(psFtraceData->hGPUTraceTLStream);

	/* If we have a valid stream attempt to acquire some data */
	eError = TLClientAcquireData(DIRECT_BRIDGE_HANDLE, psFtraceData->hGPUTraceTLStream, &pBuffer, &ui32ReadLen);
	if (eError == PVRSRV_OK)
	{
		/* Process the HWPerf packets and release the data */
		if (ui32ReadLen > 0)
		{
			PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfFTraceGPUThread: DATA AVAILABLE offset=%p, length=%d", pBuffer, ui32ReadLen));

			/* Process the transport layer data for HWPerf packets... */
			RGXHWPerfFTraceGPUProcessPackets(psDeviceInfo, pBuffer, ui32ReadLen);

			eError = TLClientReleaseData(DIRECT_BRIDGE_HANDLE, psFtraceData->hGPUTraceTLStream);
			if (eError != PVRSRV_OK)
			{
				PVR_LOG_ERROR(eError, "TLClientReleaseData");

				/* Serious error, disable FTrace GPU events */

				/* Release TraceLock so we always have the locking
				 * order BridgeLock->TraceLock to prevent AB-BA deadlocks*/
				OSLockRelease(psFtraceData->hFTraceResourceLock);
#if defined(PVRSRV_USE_BRIDGE_LOCK)
				OSAcquireBridgeLock();
#endif
				OSLockAcquire(psFtraceData->hFTraceResourceLock);
				RGXHWPerfFTraceGPUDisable(psDeviceInfo, IMG_FALSE);
				OSLockRelease(psFtraceData->hFTraceResourceLock);
#if defined(PVRSRV_USE_BRIDGE_LOCK)
				OSReleaseBridgeLock();
#endif
				goto out;

			}
		} /* else no data, ignore */
	}
	else if (eError != PVRSRV_ERROR_TIMEOUT)
	{
		PVR_LOG_ERROR(eError, "TLClientAcquireData");
	}

	OSLockRelease(psFtraceData->hFTraceResourceLock);
out:
	PVR_DPF_RETURN;
}

inline PVRSRV_ERROR RGXHWPerfFTraceGPUInitSupport(void)
{
	PVRSRV_ERROR eError;

	if (ghLockFTraceEventLock != NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "FTrace Support is already initialized"));
		return PVRSRV_OK;
	}

	/* common module params initialization */
	eError = OSLockCreate(&ghLockFTraceEventLock, LOCK_TYPE_PASSIVE);
	PVR_LOGR_IF_ERROR(eError, "OSLockCreate");

	return PVRSRV_OK;
}

inline void RGXHWPerfFTraceGPUDeInitSupport(void)
{
	if (ghLockFTraceEventLock)
	{
		OSLockDestroy(ghLockFTraceEventLock);
		ghLockFTraceEventLock = NULL;
	}
}

PVRSRV_ERROR RGXHWPerfFTraceGPUInitDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVRSRV_ERROR eError;
	RGX_HWPERF_FTRACE_DATA *psData;
	PVRSRV_RGXDEV_INFO *psDevInfo = psDeviceNode->pvDevice;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	psData = OSAllocZMem(sizeof(RGX_HWPERF_FTRACE_DATA));
	psDevInfo->pvGpuFtraceData = psData;
	if (psData == NULL)
	{
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}

	/* We initialise it only once because we want to track if any
	 * packets were dropped. */
	psData->ui32FTraceLastOrdinal = IMG_UINT32_MAX - 1;

	eError = OSLockCreate(&psData->hFTraceResourceLock, LOCK_TYPE_DISPATCH);
	PVR_LOGR_IF_ERROR(eError, "OSLockCreate");

	return PVRSRV_OK;
}

void RGXHWPerfFTraceGPUDeInitDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVRSRV_RGXDEV_INFO *psDevInfo = psDeviceNode->pvDevice;
	RGX_HWPERF_FTRACE_DATA *psData = psDevInfo->pvGpuFtraceData;

	PVRSRV_VZ_RETN_IF_MODE(DRIVER_MODE_GUEST);
	if (psData)
	{
		/* first disable the tracing, to free up TL resources */
		if (psData->hFTraceResourceLock)
		{
			OSLockAcquire(psData->hFTraceResourceLock);
			RGXHWPerfFTraceGPUDisable(psDeviceNode->pvDevice, IMG_TRUE);
			OSLockRelease(psData->hFTraceResourceLock);

			/* now free all the FTrace resources */
			OSLockDestroy(psData->hFTraceResourceLock);
		}
		OSFreeMem(psData);
		psDevInfo->pvGpuFtraceData = NULL;
	}
}

void PVRGpuTraceEnableUfoCallback(void)
{
	PVRSRV_ERROR eError;
	PVRSRV_DEVICE_NODE *psDeviceNode = PVRSRVGetPVRSRVData()->psDeviceNodeList;
	PVRSRV_RGXDEV_INFO *psRgxDevInfo;

	/* Lock down events state, for consistent value of guiUfoEventRef */
	OSLockAcquire(ghLockFTraceEventLock);
	if (guiUfoEventRef++ == 0)
	{
		/* make sure UFO events are enabled on all rogue devices */
		while (psDeviceNode)
		{
			IMG_UINT64 ui64Filter;

			psRgxDevInfo = psDeviceNode->pvDevice;
			ui64Filter = RGX_HWPERF_EVENT_MASK_VALUE(RGX_HWPERF_UFO) |
							psRgxDevInfo->ui64HWPerfFilter;
			/* Small chance exists that ui64HWPerfFilter can be changed here and
			 * the newest filter value will be changed to the old one + UFO event.
			 * This is not a critical problem. */
			eError = PVRSRVRGXCtrlHWPerfKM(NULL, psDeviceNode, RGX_HWPERF_STREAM_ID0_FW,
											IMG_FALSE, ui64Filter);
			if (eError == PVRSRV_ERROR_NOT_INITIALISED)
			{
				/* If we land here that means that the FW is not initialised yet.
				 * We stored the filter and it will be passed to the firmware
				 * during its initialisation phase. So ignore. */
			}
			else if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "Could not enable UFO HWPerf events on device %d", psDeviceNode->sDevId.i32UMIdentifier));
			}

			psDeviceNode = psDeviceNode->psNext;
		}
	}
	OSLockRelease(ghLockFTraceEventLock);
}

void PVRGpuTraceDisableUfoCallback(void)
{
	PVRSRV_ERROR eError;
	PVRSRV_DEVICE_NODE *psDeviceNode;

	/* We have to check if lock is valid because on driver unload
	 * RGXHWPerfFTraceGPUDeInit is called before kernel disables the ftrace
	 * events. This means that the lock will be destroyed before this callback
	 * is called.
	 * We can safely return if that situation happens because driver will be
	 * unloaded so we don't care about HWPerf state anymore. */
	if (ghLockFTraceEventLock == NULL)
		return;

	psDeviceNode = PVRSRVGetPVRSRVData()->psDeviceNodeList;

	/* Lock down events state, for consistent value of guiUfoEventRef */
	OSLockAcquire(ghLockFTraceEventLock);
	if (--guiUfoEventRef == 0)
	{
		/* make sure UFO events are disabled on all rogue devices */
		while (psDeviceNode)
		{
			IMG_UINT64 ui64Filter;
			PVRSRV_RGXDEV_INFO *psRgxDevInfo = psDeviceNode->pvDevice;

			ui64Filter = ~(RGX_HWPERF_EVENT_MASK_VALUE(RGX_HWPERF_UFO)) &
					psRgxDevInfo->ui64HWPerfFilter;
			/* Small chance exists that ui64HWPerfFilter can be changed here and
			 * the newest filter value will be changed to the old one + UFO event.
			 * This is not a critical problem. */
			eError = PVRSRVRGXCtrlHWPerfKM(NULL, psDeviceNode, RGX_HWPERF_STREAM_ID0_FW,
			                               IMG_FALSE, ui64Filter);
			if (eError == PVRSRV_ERROR_NOT_INITIALISED)
			{
				/* If we land here that means that the FW is not initialised yet.
				 * We stored the filter and it will be passed to the firmware
				 * during its initialisation phase. So ignore. */
			}
			else if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "Could not disable UFO HWPerf events on device %d",
				        psDeviceNode->sDevId.i32UMIdentifier));
			}
			psDeviceNode = psDeviceNode->psNext;
		}
	}
	OSLockRelease(ghLockFTraceEventLock);
}

void PVRGpuTraceEnableFirmwareActivityCallback(void)
{
	PVRSRV_DEVICE_NODE *psDeviceNode = PVRSRVGetPVRSRVData()->psDeviceNodeList;
	PVRSRV_RGXDEV_INFO *psRgxDevInfo;
	uint64_t ui64Filter, ui64FWEventsFilter = 0;
	int i;

	for (i = RGX_HWPERF_FW_EVENT_RANGE_FIRST_TYPE;
		 i <= RGX_HWPERF_FW_EVENT_RANGE_LAST_TYPE; i++)
	{
		ui64FWEventsFilter |= RGX_HWPERF_EVENT_MASK_VALUE(i);
	}

	OSLockAcquire(ghLockFTraceEventLock);
	/* Enable all FW events on all the devices */
	while (psDeviceNode)
	{
		PVRSRV_ERROR eError;
		psRgxDevInfo = psDeviceNode->pvDevice;
		ui64Filter = psRgxDevInfo->ui64HWPerfFilter | ui64FWEventsFilter;

		eError = PVRSRVRGXCtrlHWPerfKM(NULL, psDeviceNode, RGX_HWPERF_STREAM_ID0_FW,
		                               IMG_FALSE, ui64Filter);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "Could not enable HWPerf event for firmware"
			        " task timings (%s).", PVRSRVGetErrorStringKM(eError)));
		}
		psDeviceNode = psDeviceNode->psNext;
	}
	OSLockRelease(ghLockFTraceEventLock);
}

void PVRGpuTraceDisableFirmwareActivityCallback(void)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;
	IMG_UINT64 ui64FWEventsFilter = ~0;
	int i;

	/* We have to check if lock is valid because on driver unload
	 * RGXHWPerfFTraceGPUDeInit is called before kernel disables the ftrace
	 * events. This means that the lock will be destroyed before this callback
	 * is called.
	 * We can safely return if that situation happens because driver will be
	 * unloaded so we don't care about HWPerf state anymore. */
	if (ghLockFTraceEventLock == NULL)
		return;

	psDeviceNode = PVRSRVGetPVRSRVData()->psDeviceNodeList;

	for (i = RGX_HWPERF_FW_EVENT_RANGE_FIRST_TYPE;
		 i <= RGX_HWPERF_FW_EVENT_RANGE_LAST_TYPE; i++)
	{
		ui64FWEventsFilter &= ~RGX_HWPERF_EVENT_MASK_VALUE(i);
	}

	OSLockAcquire(ghLockFTraceEventLock);

	/* Disable all FW events on all the devices */
	while (psDeviceNode)
	{
		PVRSRV_RGXDEV_INFO *psRgxDevInfo = psDeviceNode->pvDevice;
		IMG_UINT64 ui64Filter = psRgxDevInfo->ui64HWPerfFilter & ui64FWEventsFilter;

		if (PVRSRVRGXCtrlHWPerfKM(NULL, psDeviceNode, RGX_HWPERF_STREAM_ID0_FW,
		                          IMG_FALSE, ui64Filter) != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "Could not disable HWPerf event for firmware task timings."));
		}
		psDeviceNode = psDeviceNode->psNext;
	}

	OSLockRelease(ghLockFTraceEventLock);
}

#endif /* SUPPORT_GPUTRACE_EVENTS */

/******************************************************************************
 * Currently only implemented on Linux. Feature can be enabled to provide
 * an interface to 3rd-party kernel modules that wish to access the
 * HWPerf data. The API is documented in the rgxapi_km.h header and
 * the rgx_hwperf* headers.
 *****************************************************************************/

/* Internal HWPerf kernel connection/device data object to track the state
 * of a client session.
 */
typedef struct
{
	PVRSRV_DEVICE_NODE* psRgxDevNode;
	PVRSRV_RGXDEV_INFO* psRgxDevInfo;

	/* TL Open/close state */
	IMG_HANDLE          hSD[RGX_HWPERF_MAX_STREAM_ID];

	/* TL Acquire/release state */
	IMG_PBYTE			pHwpBuf[RGX_HWPERF_MAX_STREAM_ID];			/*!< buffer returned to user in acquire call */
	IMG_PBYTE			pHwpBufEnd[RGX_HWPERF_MAX_STREAM_ID];		/*!< pointer to end of HwpBuf */
	IMG_PBYTE			pTlBuf[RGX_HWPERF_MAX_STREAM_ID];			/*!< buffer obtained via TlAcquireData */
	IMG_PBYTE			pTlBufPos[RGX_HWPERF_MAX_STREAM_ID];		/*!< initial position in TlBuf to acquire packets */
	IMG_PBYTE			pTlBufRead[RGX_HWPERF_MAX_STREAM_ID];		/*!< pointer to the last packet read */
	IMG_UINT32			ui32AcqDataLen[RGX_HWPERF_MAX_STREAM_ID];	/*!< length of acquired TlBuf */
	IMG_BOOL			bRelease[RGX_HWPERF_MAX_STREAM_ID];		/*!< used to determine whether or not to release currently held TlBuf */


} RGX_KM_HWPERF_DEVDATA;

PVRSRV_ERROR RGXHWPerfLazyConnect(RGX_HWPERF_CONNECTION** ppsHWPerfConnection)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PVRSRV_DEVICE_NODE *psDeviceNode;
	RGX_KM_HWPERF_DEVDATA *psDevData;
	RGX_HWPERF_DEVICE *psNewHWPerfDevice;
	RGX_HWPERF_CONNECTION* psHWPerfConnection;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* avoid uninitialised data */
	PVR_ASSERT(*ppsHWPerfConnection == NULL);
	PVR_ASSERT(psPVRSRVData);

	/* Allocate connection object */
	psHWPerfConnection = OSAllocZMem(sizeof(*psHWPerfConnection));
	if (!psHWPerfConnection)
	{
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	/* early save the return pointer to aid clean-up if failure occurs */
	*ppsHWPerfConnection = psHWPerfConnection;

	psDeviceNode = psPVRSRVData->psDeviceNodeList;
	while(psDeviceNode)
	{
		/* Create a list node to be attached to connection object's list */
		psNewHWPerfDevice = OSAllocMem(sizeof(*psNewHWPerfDevice));
		if (!psNewHWPerfDevice)
		{
			return PVRSRV_ERROR_OUT_OF_MEMORY;
		}
		/* Insert node at head of the list */
		psNewHWPerfDevice->psNext = psHWPerfConnection->psHWPerfDevList;
		psHWPerfConnection->psHWPerfDevList = psNewHWPerfDevice;

		/* create a device data object for kernel server */
		psDevData = OSAllocZMem(sizeof(*psDevData));
		psNewHWPerfDevice->hDevData = (IMG_HANDLE)psDevData;
		if (!psDevData)
		{
			return PVRSRV_ERROR_OUT_OF_MEMORY;
		}
		if (OSSNPrintf(psNewHWPerfDevice->pszName, sizeof(psNewHWPerfDevice->pszName),
		               "hwperf_device_%d", psDeviceNode->sDevId.i32UMIdentifier) < 0)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Failed to form HWPerf device name for device %d",
		    	                    __FUNCTION__,
									psDeviceNode->sDevId.i32UMIdentifier));
			return PVRSRV_ERROR_INVALID_PARAMS;
		}

		psDevData->psRgxDevNode = psDeviceNode;
		psDevData->psRgxDevInfo = psDeviceNode->pvDevice;

		psDeviceNode = psDeviceNode->psNext;
	}

	return PVRSRV_OK;
}

PVRSRV_ERROR RGXHWPerfOpen(RGX_HWPERF_CONNECTION *psHWPerfConnection)
{
	RGX_KM_HWPERF_DEVDATA *psDevData;
	RGX_HWPERF_DEVICE *psHWPerfDev;
	PVRSRV_RGXDEV_INFO *psRgxDevInfo;
	PVRSRV_ERROR eError;
	IMG_CHAR pszHWPerfFwStreamName[sizeof(PVRSRV_TL_HWPERF_RGX_FW_STREAM) + 5];
	IMG_CHAR pszHWPerfHostStreamName[sizeof(PVRSRV_TL_HWPERF_HOST_SERVER_STREAM) + 5];
	IMG_UINT32 ui32BufSize;

	/* Disable producer callback by default for the Kernel API. */
	IMG_UINT32 ui32StreamFlags = PVRSRV_STREAM_FLAG_ACQUIRE_NONBLOCKING |
			                     PVRSRV_STREAM_FLAG_DISABLE_PRODUCER_CALLBACK;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Validate input argument values supplied by the caller */
	if (!psHWPerfConnection)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psHWPerfDev = psHWPerfConnection->psHWPerfDevList;
	while (psHWPerfDev)
	{
		psDevData = (RGX_KM_HWPERF_DEVDATA *) psHWPerfDev->hDevData;
		psRgxDevInfo = psDevData->psRgxDevInfo;

		/* In the case where the AppHint has not been set we need to
		 * initialise the HWPerf resources here. Allocated on-demand
		 * to reduce RAM foot print on systems not needing HWPerf.
		 */
		OSLockAcquire(psRgxDevInfo->hHWPerfLock);
		if (RGXHWPerfIsInitRequired(psRgxDevInfo))
		{
			eError = RGXHWPerfInitOnDemandResources(psRgxDevInfo);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: Initialization of on-demand HWPerfFW"
			        " resources failed", __FUNCTION__));
				OSLockRelease(psRgxDevInfo->hHWPerfLock);
				return eError;
			}
		}
		OSLockRelease(psRgxDevInfo->hHWPerfLock);

		OSLockAcquire(psRgxDevInfo->hLockHWPerfHostStream);
		if (psRgxDevInfo->hHWPerfHostStream == NULL)
		{
			eError = RGXHWPerfHostInitOnDemandResources(psRgxDevInfo);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: Initialization of on-demand HWPerfHost"
						" resources failed", __FUNCTION__));
				OSLockRelease(psRgxDevInfo->hLockHWPerfHostStream);
				return eError;
			}
		}
		OSLockRelease(psRgxDevInfo->hLockHWPerfHostStream);

		/* form the HWPerf stream name, corresponding to this DevNode; which can make sense in the UM */
		if (OSSNPrintf(pszHWPerfFwStreamName, sizeof(pszHWPerfFwStreamName), "%s%d",
						PVRSRV_TL_HWPERF_RGX_FW_STREAM,
						psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier) < 0)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Failed to form HWPerf stream name for device %d",
		    	                    __FUNCTION__,
									psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier));
			return PVRSRV_ERROR_INVALID_PARAMS;
		}
		/* Open the RGX TL stream for reading in this session */
		eError = TLClientOpenStream(DIRECT_BRIDGE_HANDLE,
									pszHWPerfFwStreamName,
									ui32StreamFlags,
									&psDevData->hSD[RGX_HWPERF_STREAM_ID0_FW]);
		PVR_LOGR_IF_ERROR(eError, "TLClientOpenStream(RGX_HWPerf)");

		/* form the HWPerf host stream name, corresponding to this DevNode; which can make sense in the UM */
		if (OSSNPrintf(pszHWPerfHostStreamName, sizeof(pszHWPerfHostStreamName), "%s%d",
					PVRSRV_TL_HWPERF_HOST_SERVER_STREAM,
					psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier) < 0)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Failed to form HWPerf host stream name for device %d",
		    	                    __FUNCTION__,
									psRgxDevInfo->psDeviceNode->sDevId.i32UMIdentifier));
			return PVRSRV_ERROR_INVALID_PARAMS;
		}

		/* Open the host TL stream for reading in this session */
		eError = TLClientOpenStream(DIRECT_BRIDGE_HANDLE,
									pszHWPerfHostStreamName,
									PVRSRV_STREAM_FLAG_ACQUIRE_NONBLOCKING,
									&psDevData->hSD[RGX_HWPERF_STREAM_ID1_HOST]);
		PVR_LOGR_IF_ERROR(eError, "TLClientOpenStream(Host_HWPerf)");

		/* Allocate a large enough buffer for use during the entire session to
		 * avoid the need to resize in the Acquire call as this might be in an ISR
		 * Choose size that can contain at least one packet.
		 */
		/* Allocate buffer for FW Stream */
		ui32BufSize = FW_STREAM_BUFFER_SIZE;
		psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID0_FW] = OSAllocMem(ui32BufSize);
		if (psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID0_FW] == NULL)
		{
			return PVRSRV_ERROR_OUT_OF_MEMORY;
		}
		psDevData->pHwpBufEnd[RGX_HWPERF_STREAM_ID0_FW] = psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID0_FW]+ui32BufSize;

		/* Allocate buffer for Host Stream */
		ui32BufSize = HOST_STREAM_BUFFER_SIZE;
		psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID1_HOST] = OSAllocMem(ui32BufSize);
		if (psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID1_HOST] == NULL)
		{
			OSFreeMem(psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID0_FW]);
			return PVRSRV_ERROR_OUT_OF_MEMORY;
		}
		psDevData->pHwpBufEnd[RGX_HWPERF_STREAM_ID1_HOST] = psDevData->pHwpBuf[RGX_HWPERF_STREAM_ID1_HOST]+ui32BufSize;

		psHWPerfDev = psHWPerfDev->psNext;
	}

	return PVRSRV_OK;
}


PVRSRV_ERROR RGXHWPerfConnect(RGX_HWPERF_CONNECTION** ppsHWPerfConnection)
{
	PVRSRV_ERROR eError;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	eError = RGXHWPerfLazyConnect(ppsHWPerfConnection);
	PVR_LOGG_IF_ERROR(eError, "RGXHWPerfLazyConnect", e0);

	eError = RGXHWPerfOpen(*ppsHWPerfConnection);
	PVR_LOGG_IF_ERROR(eError, "RGXHWPerfOpen", e1);

	return PVRSRV_OK;

e1: /* HWPerfOpen might have opened some, and then failed */
	RGXHWPerfClose(*ppsHWPerfConnection);
e0: /* LazyConnect might have allocated some resources and then failed,
	 * make sure they are cleaned up */
	RGXHWPerfFreeConnection(ppsHWPerfConnection);
	return eError;
}


PVRSRV_ERROR RGXHWPerfControl(
		RGX_HWPERF_CONNECTION *psHWPerfConnection,
		RGX_HWPERF_STREAM_ID eStreamId,
		IMG_BOOL             bToggle,
		IMG_UINT64           ui64Mask)
{
	PVRSRV_ERROR           eError;
	RGX_KM_HWPERF_DEVDATA* psDevData;
	RGX_HWPERF_DEVICE* psHWPerfDev;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Validate input argument values supplied by the caller */
	if (!psHWPerfConnection)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psHWPerfDev = psHWPerfConnection->psHWPerfDevList;

	while (psHWPerfDev)
	{
		psDevData = (RGX_KM_HWPERF_DEVDATA *) psHWPerfDev->hDevData;

		/* Call the internal server API */
		eError = PVRSRVRGXCtrlHWPerfKM(NULL, psDevData->psRgxDevNode, eStreamId, bToggle, ui64Mask);
		PVR_LOGR_IF_ERROR(eError, "PVRSRVRGXCtrlHWPerfKM");

		psHWPerfDev = psHWPerfDev->psNext;
	}

	return PVRSRV_OK;
}


PVRSRV_ERROR RGXHWPerfConfigureAndEnableCounters(
		RGX_HWPERF_CONNECTION *psHWPerfConnection,
		IMG_UINT32					ui32NumBlocks,
		RGX_HWPERF_CONFIG_CNTBLK*	asBlockConfigs)
{
	PVRSRV_ERROR           eError = PVRSRV_OK;
	RGX_KM_HWPERF_DEVDATA* psDevData;
	RGX_HWPERF_DEVICE *psHWPerfDev;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Validate input argument values supplied by the caller */
	if (!psHWPerfConnection || ui32NumBlocks==0 || !asBlockConfigs)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (ui32NumBlocks > RGXFWIF_HWPERF_CTRL_BLKS_MAX)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psHWPerfDev = psHWPerfConnection->psHWPerfDevList;

	while (psHWPerfDev)
	{
		psDevData = (RGX_KM_HWPERF_DEVDATA *) psHWPerfDev->hDevData;

		/* Call the internal server API */
		eError = PVRSRVRGXConfigEnableHWPerfCountersKM(NULL,
				psDevData->psRgxDevNode, ui32NumBlocks, asBlockConfigs);
		PVR_LOGR_IF_ERROR(eError, "PVRSRVRGXCtrlHWPerfKM");

		psHWPerfDev = psHWPerfDev->psNext;
	}

	return eError;
}


PVRSRV_ERROR RGXHWPerfDisableCounters(
		RGX_HWPERF_CONNECTION *psHWPerfConnection,
		IMG_UINT32   ui32NumBlocks,
		IMG_UINT16*   aeBlockIDs)
{
	PVRSRV_ERROR           eError = PVRSRV_OK;
	RGX_KM_HWPERF_DEVDATA* psDevData;
	RGX_HWPERF_DEVICE* psHWPerfDev;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Validate input argument values supplied by the caller */
	if (!psHWPerfConnection || ui32NumBlocks==0 || !aeBlockIDs)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (ui32NumBlocks > RGXFWIF_HWPERF_CTRL_BLKS_MAX)
    {
        return PVRSRV_ERROR_INVALID_PARAMS;
    }

	psHWPerfDev = psHWPerfConnection->psHWPerfDevList;

	while (psHWPerfDev)
	{
		psDevData = (RGX_KM_HWPERF_DEVDATA *) psHWPerfDev->hDevData;

		/* Call the internal server API */
		eError = PVRSRVRGXCtrlHWPerfCountersKM(NULL,
					psDevData->psRgxDevNode, IMG_FALSE, ui32NumBlocks, aeBlockIDs);
		PVR_LOGR_IF_ERROR(eError, "PVRSRVRGXCtrlHWPerfCountersKM");

		psHWPerfDev = psHWPerfDev->psNext;
	}

	return eError;
}


PVRSRV_ERROR RGXHWPerfAcquireEvents(
		IMG_HANDLE  hDevData,
		RGX_HWPERF_STREAM_ID eStreamId,
		IMG_PBYTE*  ppBuf,
		IMG_UINT32* pui32BufLen)
{
	PVRSRV_ERROR			eError;
	RGX_KM_HWPERF_DEVDATA*	psDevData = (RGX_KM_HWPERF_DEVDATA*)hDevData;
	IMG_PBYTE				pDataDest;
	IMG_UINT32			ui32TlPackets = 0;
	IMG_PBYTE			pBufferEnd;
	PVRSRVTL_PPACKETHDR psHDRptr;
	PVRSRVTL_PACKETTYPE ui16TlType;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Reset the output arguments in case we discover an error */
	*ppBuf = NULL;
	*pui32BufLen = 0;

	/* Valid input argument values supplied by the caller */
	if (!psDevData || eStreamId >= RGX_HWPERF_MAX_STREAM_ID)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (psDevData->pTlBuf[eStreamId] == NULL)
	{
		/* Acquire some data to read from the HWPerf TL stream */
		eError = TLClientAcquireData(DIRECT_BRIDGE_HANDLE,
								 	 psDevData->hSD[eStreamId],
									 &psDevData->pTlBuf[eStreamId],
									 &psDevData->ui32AcqDataLen[eStreamId]);
		PVR_LOGR_IF_ERROR(eError, "TLClientAcquireData");

		psDevData->pTlBufPos[eStreamId] = psDevData->pTlBuf[eStreamId];
	}

	/* TL indicates no data exists so return OK and zero. */
	if ((psDevData->pTlBufPos[eStreamId] == NULL) || (psDevData->ui32AcqDataLen[eStreamId] == 0))
	{
		return PVRSRV_OK;
	}

	/* Process each TL packet in the data buffer we have acquired */
	pBufferEnd = psDevData->pTlBuf[eStreamId]+psDevData->ui32AcqDataLen[eStreamId];
	pDataDest = psDevData->pHwpBuf[eStreamId];
	psHDRptr = GET_PACKET_HDR(psDevData->pTlBufPos[eStreamId]);
	psDevData->pTlBufRead[eStreamId] = psDevData->pTlBufPos[eStreamId];
	while ( psHDRptr < (PVRSRVTL_PPACKETHDR)pBufferEnd )
	{
		ui16TlType = GET_PACKET_TYPE(psHDRptr);
		if (ui16TlType == PVRSRVTL_PACKETTYPE_DATA)
		{
			IMG_UINT16 ui16DataLen = GET_PACKET_DATA_LEN(psHDRptr);
			if (0 == ui16DataLen)
			{
				PVR_DPF((PVR_DBG_ERROR, "RGXHWPerfAcquireEvents: ZERO Data in TL data packet: %p", psHDRptr));
			}
			else
			{
				/* Check next packet does not fill buffer */
				if (pDataDest + ui16DataLen > psDevData->pHwpBufEnd[eStreamId])
				{
					break;
				}

				/* For valid data copy it into the client buffer and move
				 * the write position on */
				OSDeviceMemCopy(pDataDest, GET_PACKET_DATA_PTR(psHDRptr), ui16DataLen);
				pDataDest += ui16DataLen;
			}
		}
		else if (ui16TlType == PVRSRVTL_PACKETTYPE_MOST_RECENT_WRITE_FAILED)
		{
			PVR_DPF((PVR_DBG_MESSAGE, "RGXHWPerfAcquireEvents: Indication that the transport buffer was full"));
		}
		else
		{
			/* else Ignore padding packet type and others */
			PVR_DPF((PVR_DBG_MESSAGE, "RGXHWPerfAcquireEvents: Ignoring TL packet, type %d", ui16TlType ));
		}

		/* Update loop variable to the next packet and increment counts */
		psHDRptr = GET_NEXT_PACKET_ADDR(psHDRptr);
		/* Updated to keep track of the next packet to be read. */
		psDevData->pTlBufRead[eStreamId] = (IMG_PBYTE) psHDRptr;
		ui32TlPackets++;
	}

	PVR_DPF((PVR_DBG_VERBOSE, "RGXHWPerfAcquireEvents: TL Packets processed %03d", ui32TlPackets));

	psDevData->bRelease[eStreamId] = IMG_FALSE;
	if (psHDRptr >= (PVRSRVTL_PPACKETHDR) pBufferEnd)
	{
		psDevData->bRelease[eStreamId] = IMG_TRUE;
	}

	/* Update output arguments with client buffer details and true length */
	*ppBuf = psDevData->pHwpBuf[eStreamId];
	*pui32BufLen = pDataDest - psDevData->pHwpBuf[eStreamId];

	return PVRSRV_OK;
}


PVRSRV_ERROR RGXHWPerfReleaseEvents(
		IMG_HANDLE hDevData,
		RGX_HWPERF_STREAM_ID eStreamId)
{
	PVRSRV_ERROR			eError = PVRSRV_OK;
	RGX_KM_HWPERF_DEVDATA*	psDevData = (RGX_KM_HWPERF_DEVDATA*)hDevData;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Valid input argument values supplied by the caller */
	if (!psDevData || eStreamId >= RGX_HWPERF_MAX_STREAM_ID)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (psDevData->bRelease[eStreamId])
	{
		/* Inform the TL that we are done with reading the data. */
		eError = TLClientReleaseData(DIRECT_BRIDGE_HANDLE, psDevData->hSD[eStreamId]);
		psDevData->ui32AcqDataLen[eStreamId] = 0;
		psDevData->pTlBuf[eStreamId] = NULL;
	}
	else
	{
		psDevData->pTlBufPos[eStreamId] = psDevData->pTlBufRead[eStreamId];
	}
	return eError;
}


PVRSRV_ERROR RGXHWPerfGetFilter(
		IMG_HANDLE  hDevData,
		RGX_HWPERF_STREAM_ID eStreamId,
		IMG_UINT64 *ui64Filter)
{
	PVRSRV_RGXDEV_INFO* psRgxDevInfo;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	/* Valid input argument values supplied by the caller */
	psRgxDevInfo = hDevData ? ((RGX_KM_HWPERF_DEVDATA*) hDevData)->psRgxDevInfo : NULL;
	if (!psRgxDevInfo)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Invalid pointer to the RGX device",
		        __func__));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* No need to take hHWPerfLock here since we are only reading data
	 * from always existing integers to return to debugfs which is an
	 * atomic operation.
	 */
	switch (eStreamId) {
		case RGX_HWPERF_STREAM_ID0_FW:
			*ui64Filter = psRgxDevInfo->ui64HWPerfFilter;
			break;
		case RGX_HWPERF_STREAM_ID1_HOST:
			*ui64Filter = psRgxDevInfo->ui32HWPerfHostFilter;
			break;
		default:
			PVR_DPF((PVR_DBG_ERROR, "%s: Invalid stream ID",
			        __func__));
			return PVRSRV_ERROR_INVALID_PARAMS;
	}

	return PVRSRV_OK;
}


PVRSRV_ERROR RGXHWPerfFreeConnection(RGX_HWPERF_CONNECTION** ppsHWPerfConnection)
{
	RGX_HWPERF_DEVICE *psHWPerfDev, *psHWPerfNextDev;
	RGX_HWPERF_CONNECTION *psHWPerfConnection = *ppsHWPerfConnection;

	/* if connection object itself is NULL, nothing to free */
	if (psHWPerfConnection == NULL)
	{
		return PVRSRV_OK;
	}

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	psHWPerfNextDev = psHWPerfConnection->psHWPerfDevList;
	while (psHWPerfNextDev)
	{
		psHWPerfDev = psHWPerfNextDev;
		psHWPerfNextDev = psHWPerfNextDev->psNext;

		/* Free the session memory */
		if (psHWPerfDev->hDevData)
			OSFreeMem(psHWPerfDev->hDevData);
		OSFreeMem(psHWPerfDev);
	}
	OSFreeMem(psHWPerfConnection);
	*ppsHWPerfConnection = NULL;

	return PVRSRV_OK;
}


PVRSRV_ERROR RGXHWPerfClose(RGX_HWPERF_CONNECTION *psHWPerfConnection)
{
	RGX_HWPERF_DEVICE *psHWPerfDev;
	RGX_KM_HWPERF_DEVDATA* psDevData;
	IMG_UINT uiStreamId;
	PVRSRV_ERROR eError;

	/* Check session connection is not zero */
	if (!psHWPerfConnection)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	psHWPerfDev = psHWPerfConnection->psHWPerfDevList;
	while (psHWPerfDev)
	{
		psDevData = (RGX_KM_HWPERF_DEVDATA *) psHWPerfDev->hDevData;
		for (uiStreamId = 0; uiStreamId < RGX_HWPERF_MAX_STREAM_ID; uiStreamId++)
		{
			/* If the TL buffer exists they have not called ReleaseData
			 * before disconnecting so clean it up */
			if (psDevData->pTlBuf[uiStreamId])
			{
				/* TLClientReleaseData call and null out the buffer fields
				 * and length */
				eError = TLClientReleaseData(DIRECT_BRIDGE_HANDLE, psDevData->hSD[uiStreamId]);
				psDevData->ui32AcqDataLen[uiStreamId] = 0;
				psDevData->pTlBuf[uiStreamId] = NULL;
				PVR_LOG_IF_ERROR(eError, "TLClientReleaseData");
				/* Packets may be lost if release was not required */
				if (!psDevData->bRelease[uiStreamId])
				{
					PVR_DPF((PVR_DBG_WARNING, "RGXHWPerfClose: Events in buffer waiting to be read, remaining events may be lost."));
				}
			}

			/* Close the TL stream, ignore the error if it occurs as we
			 * are disconnecting */
			if (psDevData->hSD[uiStreamId])
			{
				eError = TLClientCloseStream(DIRECT_BRIDGE_HANDLE,
											 psDevData->hSD[uiStreamId]);
				PVR_LOG_IF_ERROR(eError, "TLClientCloseStream");
				psDevData->hSD[uiStreamId] = NULL;
			}

			/* Free the client buffer used in session */
			if (psDevData->pHwpBuf[uiStreamId])
			{
				OSFreeMem(psDevData->pHwpBuf[uiStreamId]);
				psDevData->pHwpBuf[uiStreamId] = NULL;
			}
		}
		psHWPerfDev = psHWPerfDev->psNext;
	}

	return PVRSRV_OK;
}


PVRSRV_ERROR RGXHWPerfDisconnect(RGX_HWPERF_CONNECTION** ppsHWPerfConnection)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	PVRSRV_VZ_RET_IF_MODE(DRIVER_MODE_GUEST, PVRSRV_ERROR_NOT_IMPLEMENTED);

	eError = RGXHWPerfClose(*ppsHWPerfConnection);
	PVR_LOG_IF_ERROR(eError, "RGXHWPerfClose");

	eError = RGXHWPerfFreeConnection(ppsHWPerfConnection);
	PVR_LOG_IF_ERROR(eError, "RGXHWPerfFreeConnection");

	return eError;
}


const IMG_CHAR *RGXHWPerfKickTypeToStr(RGX_HWPERF_KICK_TYPE eKickType)
{
	static const IMG_CHAR *aszKickType[RGX_HWPERF_KICK_TYPE_LAST+1] = {
		"TA3D", "TQ2D", "TQ3D", "CDM", "RS", "VRDM", "TQTDM", "SYNC", "LAST"
	};

	/* cast in case of negative value */
	if (((IMG_UINT32) eKickType) >= RGX_HWPERF_KICK_TYPE_LAST)
	{
		return "<UNKNOWN>";
	}

	return aszKickType[eKickType];
}


IMG_UINT64 RGXHWPerfConvertCRTimeStamp(
		IMG_UINT32 ui32ClkSpeed,
		IMG_UINT64 ui64CorrCRTimeStamp,
		IMG_UINT64 ui64CorrOSTimeStamp,
		IMG_UINT64 ui64CRTimeStamp)
{
	IMG_UINT32 ui32Remainder;
	IMG_UINT64 ui64CRDeltaToOSDeltaKNs;
	IMG_UINT64 ui64EventOSTimestamp, deltaRgxTimer, delta_ns;

	if (!(ui64CRTimeStamp) || !(ui32ClkSpeed) || !(ui64CorrCRTimeStamp) || !(ui64CorrOSTimeStamp))
	{
		return 0;
	}

	ui64CRDeltaToOSDeltaKNs = RGXFWIF_GET_CRDELTA_TO_OSDELTA_K_NS(ui32ClkSpeed,
																  ui32Remainder);

	/* RGX CR timer ticks delta */
	deltaRgxTimer = ui64CRTimeStamp - ui64CorrCRTimeStamp;
	/* RGX time delta in nanoseconds */
	delta_ns = RGXFWIF_GET_DELTA_OSTIME_NS(deltaRgxTimer, ui64CRDeltaToOSDeltaKNs);
	/* Calculate OS time of HWPerf event */
	ui64EventOSTimestamp = ui64CorrOSTimeStamp + delta_ns;

	return ui64EventOSTimestamp;
}

/******************************************************************************
 End of file (rgxhwperf.c)
******************************************************************************/
