/*************************************************************************/ /*!
@File
@Title          Functions for creating debugfs directories and entries.
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
#include <linux/slab.h>

#include "pvr_debug.h"
#include "pvr_debugfs.h"
#include "allocmem.h"

#ifdef MODULE
#define PVR_DEBUGFS_DIR_NAME (PVR_DRM_NAME "_1_9")
#else
#define PVR_DEBUGFS_DIR_NAME PVR_DRM_NAME
#endif

/* Define to set the PVR_DPF debug output level for pvr_debugfs.
 * Normally, leave this set to PVR_DBGDRIV_MESSAGE, but when debugging
 * you can temporarily change this to PVR_DBG_ERROR.
 */
#if defined(PVRSRV_NEED_PVR_DPF)
#define PVR_DEBUGFS_PVR_DPF_LEVEL      PVR_DBGDRIV_MESSAGE
#else
#define PVR_DEBUGFS_PVR_DPF_LEVEL      0
#endif

static struct dentry *gpsPVRDebugFSEntryDir = NULL;

/* Lock used when adjusting refCounts and deleting entries */
static struct mutex gDebugFSLock;

/*************************************************************************/ /*!
 Statistic entry read functions
*/ /**************************************************************************/

#if defined(PVRSRV_ENABLE_MEMTRACK_STATS_FILE)
typedef struct _PVR_DEBUGFS_RAW_DRIVER_STAT_
{
	OS_STATS_PRINT_FUNC *pfStatsPrint;
	PPVR_DEBUGFS_ENTRY_DATA pvDebugFsEntry;
} PVR_DEBUGFS_RAW_DRIVER_STAT;
#endif

typedef struct _PVR_DEBUGFS_DRIVER_STAT_
{
	void				 *pvData;
	OS_STATS_PRINT_FUNC  *pfnStatsPrint;
	PVRSRV_INC_STAT_MEM_REFCOUNT_FUNC	*pfnIncStatMemRefCount;
	PVRSRV_DEC_STAT_MEM_REFCOUNT_FUNC	*pfnDecStatMemRefCount;
	IMG_UINT32				ui32RefCount;
	PPVR_DEBUGFS_ENTRY_DATA	pvDebugFSEntry;
} PVR_DEBUGFS_DRIVER_STAT;

typedef struct _PVR_DEBUGFS_DIR_DATA_
{
	struct dentry *psDir;
	PPVR_DEBUGFS_DIR_DATA psParentDir;
	IMG_UINT32	ui32RefCount;
} PVR_DEBUGFS_DIR_DATA;

typedef struct _PVR_DEBUGFS_ENTRY_DATA_
{
	struct dentry *psEntry;
	PVR_DEBUGFS_DIR_DATA *psParentDir;
	IMG_UINT32	ui32RefCount;
	PVR_DEBUGFS_DRIVER_STAT *psStatData;
} PVR_DEBUGFS_ENTRY_DATA;

typedef struct _PVR_DEBUGFS_BLOB_ENTRY_DATA_
{
	struct dentry *psEntry;
	PVR_DEBUGFS_DIR_DATA *psParentDir;
	struct debugfs_blob_wrapper blob;
} PVR_DEBUGFS_BLOB_ENTRY_DATA;

typedef struct _PVR_DEBUGFS_PRIV_DATA_
{
	const struct seq_operations *psReadOps;
	PVRSRV_ENTRY_WRITE_FUNC	*pfnWrite;
	void			*pvData;
	PVRSRV_INC_FSENTRY_PVDATA_REFCNT_FN *pfIncPvDataRefCnt;
	PVRSRV_DEC_FSENTRY_PVDATA_REFCNT_FN *pfDecPvDataRefCnt;
	IMG_BOOL		bValid;
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry;
} PVR_DEBUGFS_PRIV_DATA;

static IMG_BOOL _RefDirEntry(PVR_DEBUGFS_DIR_DATA *psDirEntry);
static inline void _UnrefAndMaybeDestroyDirEntry(PVR_DEBUGFS_DIR_DATA **ppsDirEntry);
static void _UnrefAndMaybeDestroyDirEntryWhileLocked(PVR_DEBUGFS_DIR_DATA **ppsDirEntry);
static IMG_BOOL _RefDebugFSEntryNoLock(PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry);
static void _UnrefAndMaybeDestroyDebugFSEntry(PVR_DEBUGFS_ENTRY_DATA **ppsDebugFSEntry);
static IMG_BOOL _RefStatEntry(PVR_DEBUGFS_DRIVER_STAT *psStatEntry);
static IMG_BOOL _UnrefAndMaybeDestroyStatEntry(PVR_DEBUGFS_DRIVER_STAT *psStatEntry);

static void _StatsSeqPrintf(void *pvFile, const IMG_CHAR *pszFormat, ...)
{
	IMG_CHAR  szBuffer[PVR_MAX_DEBUG_MESSAGE_LEN];
	va_list  ArgList;

	va_start(ArgList, pszFormat);
	vsnprintf(szBuffer, PVR_MAX_DEBUG_MESSAGE_LEN, pszFormat, ArgList);
	seq_printf((struct seq_file *)pvFile, "%s", szBuffer);
	va_end(ArgList);
}

static void *_DebugFSStatisticSeqStart(struct seq_file *psSeqFile, loff_t *puiPosition)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;

	if (psStatData)
	{
		/* take reference on psStatData (for duration of stat iteration) */
		if (!_RefStatEntry(psStatData))
		{
			PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called for '%s' but failed"
			        " to take ref on stat entry, returning -EIO(%d)", __func__,
			        psStatData->pvDebugFSEntry->psEntry->d_iname, -EIO));
			return NULL;
		}

		if (*puiPosition == 0)
		{
			return psStatData;
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is NULL", __FUNCTION__));
	}

	return NULL;
}

static void _DebugFSStatisticSeqStop(struct seq_file *psSeqFile, void *pvData)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;
	PVR_UNREFERENCED_PARAMETER(pvData);

	if (psStatData)
	{
		/* drop ref taken on stat memory, and if it is now zero, be sure we don't try to read it again */
		if (psStatData->ui32RefCount > 0)
		{
			/* drop reference on psStatData (held for duration of stat iteration) */
			_UnrefAndMaybeDestroyStatEntry((void*)psStatData);
		}
		else
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: PVR_DEBUGFS_DRIVER_STAT has zero refcount",
											__func__));
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is NULL", __FUNCTION__));
	}
}

static void *_DebugFSStatisticSeqNext(struct seq_file *psSeqFile,
				      void *pvData,
				      loff_t *puiPosition)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;
	PVR_UNREFERENCED_PARAMETER(pvData);

	if (psStatData)
	{
		if (psStatData->pvData)
		{
			if (puiPosition)
			{
				(*puiPosition)++;
			}
			else
			{
				PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called with puiPosition NULL", __FUNCTION__));
			}
		}
		else
		{
			/* psStatData->pvData is NULL */
			/* NB This is valid if the stat has no structure associated with it (eg. driver_stats, which prints totals stored in a number of global vars) */
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is NULL", __FUNCTION__));
	}

	return NULL;
}

static int _DebugFSStatisticSeqShow(struct seq_file *psSeqFile, void *pvData)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)pvData;

	if (psStatData != NULL)
	{
		psStatData->pfnStatsPrint((void*)psSeqFile, psStatData->pvData, _StatsSeqPrintf);
		return 0;
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is NULL, returning -ENODATA(%d)", __FUNCTION__, -ENODATA));
	}

	return -ENODATA;
}

static struct seq_operations gsDebugFSStatisticReadOps =
{
	.start = _DebugFSStatisticSeqStart,
	.stop  = _DebugFSStatisticSeqStop,
	.next  = _DebugFSStatisticSeqNext,
	.show  = _DebugFSStatisticSeqShow,
};


/*************************************************************************/ /*!
 Common internal API
*/ /**************************************************************************/

static int _DebugFSFileOpen(struct inode *psINode, struct file *psFile)
{
	PVR_DEBUGFS_PRIV_DATA *psPrivData;
	int iResult = -EIO;
	IMG_BOOL bRefRet = IMG_FALSE;
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry = NULL;

	mutex_lock(&gDebugFSLock);

	PVR_ASSERT(psINode);
	psPrivData = (PVR_DEBUGFS_PRIV_DATA *)psINode->i_private;

	if (psPrivData)
	{
		/* Check that psPrivData is still valid to use */
		if (psPrivData->bValid)
		{
			psDebugFSEntry = psPrivData->psDebugFSEntry;

			/* Take ref on stat entry before opening seq file - this ref will be dropped if we
			 * fail to open the seq file or when we close it
			 */
			if (psDebugFSEntry)
			{
				bRefRet = _RefDebugFSEntryNoLock(psDebugFSEntry);
				mutex_unlock(&gDebugFSLock);
				if (psPrivData->pfIncPvDataRefCnt)
				{
					psPrivData->pfIncPvDataRefCnt(psPrivData->pvData);
				}
				if (bRefRet)
				{
					iResult = seq_open(psFile, psPrivData->psReadOps);
					if (iResult == 0)
					{
						struct seq_file *psSeqFile = psFile->private_data;

						psSeqFile->private = psPrivData->pvData;
					}
					else
					{
						if (psPrivData->pfDecPvDataRefCnt)
						{
							psPrivData->pfDecPvDataRefCnt(psPrivData->pvData);
						}
						/* Drop ref if we failed to open seq file */
						_UnrefAndMaybeDestroyDebugFSEntry(&psPrivData->psDebugFSEntry);
						PVR_DPF((PVR_DBG_ERROR, "%s: Failed to seq_open psFile, returning %d", __FUNCTION__, iResult));
					}
				}
			}
			else
			{
				mutex_unlock(&gDebugFSLock);
			}
		}
		else
		{
			mutex_unlock(&gDebugFSLock);
		}
	}
	else
	{
		mutex_unlock(&gDebugFSLock);
	}

	return iResult;
}

static int _DebugFSFileClose(struct inode *psINode, struct file *psFile)
{
	int iResult;
	PVR_DEBUGFS_PRIV_DATA *psPrivData = (PVR_DEBUGFS_PRIV_DATA *)psINode->i_private;
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry = NULL;

	if (psPrivData)
	{
		psDebugFSEntry = psPrivData->psDebugFSEntry;
	}
	iResult = seq_release(psINode, psFile);
	if (psDebugFSEntry)
	{
		_UnrefAndMaybeDestroyDebugFSEntry(&psPrivData->psDebugFSEntry);
	}
	if (psPrivData->pfDecPvDataRefCnt)
	{
		psPrivData->pfDecPvDataRefCnt(psPrivData->pvData);
	}
	return iResult;
}

static ssize_t _DebugFSFileWrite(struct file *psFile,
				 const char __user *pszBuffer,
				 size_t uiCount,
				 loff_t *puiPosition)
{
	struct inode *psINode = psFile->f_path.dentry->d_inode;
	PVR_DEBUGFS_PRIV_DATA *psPrivData = (PVR_DEBUGFS_PRIV_DATA *)psINode->i_private;

	if (psPrivData->pfnWrite == NULL)
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called for file '%s', which does not have pfnWrite defined, returning -EIO(%d)", __FUNCTION__, psFile->f_path.dentry->d_iname, -EIO));
		return -EIO;
	}

	return psPrivData->pfnWrite(pszBuffer, uiCount, puiPosition, psPrivData->pvData);
}

static const struct file_operations gsPVRDebugFSFileOps =
{
	.owner = THIS_MODULE,
	.open = _DebugFSFileOpen,
	.read = seq_read,
	.write = _DebugFSFileWrite,
	.llseek = seq_lseek,
	.release = _DebugFSFileClose,
};


/*************************************************************************/ /*!
 Public API
*/ /**************************************************************************/

/*************************************************************************/ /*!
@Function       PVRDebugFSInit
@Description    Initialise PVR debugfs support. This should be called before
                using any PVRDebugFS functions.
@Return         int      On success, returns 0. Otherwise, returns an
                         error code.
*/ /**************************************************************************/
int PVRDebugFSInit(void)
{
	PVR_ASSERT(gpsPVRDebugFSEntryDir == NULL);

	mutex_init(&gDebugFSLock);

	gpsPVRDebugFSEntryDir = debugfs_create_dir(PVR_DEBUGFS_DIR_NAME, NULL);
	if (gpsPVRDebugFSEntryDir == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create '%s' debugfs root directory",
			 __FUNCTION__, PVR_DEBUGFS_DIR_NAME));

		return -ENOMEM;
	}

	return 0;
}

/*************************************************************************/ /*!
@Function       PVRDebugFSDeInit
@Description    Deinitialise PVR debugfs support. This should be called only
                if PVRDebugFSInit() has already been called. All debugfs
                directories and entries should be removed otherwise this
                function will fail.
@Return         void
*/ /**************************************************************************/
void PVRDebugFSDeInit(void)
{
	if (gpsPVRDebugFSEntryDir != NULL)
	{
		debugfs_remove(gpsPVRDebugFSEntryDir);
		gpsPVRDebugFSEntryDir = NULL;
		mutex_destroy(&gDebugFSLock);
	}
}

/*************************************************************************/ /*!
@Function		PVRDebugFSCreateEntryDir
@Description	Create a directory for debugfs entries that will be located
				under the root directory, as created by
				PVRDebugFSCreateEntries().
@Input			pszName		 String containing the name for the directory.
@Input			psParentDir	 The parent directory in which to create the new
							 directory. This should either be NULL, meaning it
							 should be created in the root directory, or a
							 pointer to a directory as returned by this
							 function.
@Output			ppsNewDir	 On success, points to the newly created
							 directory.
@Return			int			 On success, returns 0. Otherwise, returns an
							 error code.
*/ /**************************************************************************/
int PVRDebugFSCreateEntryDir(IMG_CHAR *pszName,
				 PVR_DEBUGFS_DIR_DATA *psParentDir,
				 PVR_DEBUGFS_DIR_DATA **ppsNewDir)
{
	PVR_DEBUGFS_DIR_DATA *psNewDir;

	PVR_ASSERT(gpsPVRDebugFSEntryDir != NULL);

	if (pszName == NULL || ppsNewDir == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s:   Invalid param", __FUNCTION__));
		return -EINVAL;
	}

	psNewDir = OSAllocMemNoStats(sizeof(*psNewDir));

	if (psNewDir == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot allocate memory for '%s' pvr_debugfs structure",
			 __FUNCTION__, pszName));
		return -ENOMEM;
	}

	psNewDir->psParentDir = psParentDir;
	psNewDir->psDir = debugfs_create_dir(pszName, (psNewDir->psParentDir) ? psNewDir->psParentDir->psDir : gpsPVRDebugFSEntryDir);

	if (psNewDir->psDir == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create '%s' debugfs directory",
			 __FUNCTION__, pszName));

		OSFreeMemNoStats(psNewDir);
		return -ENOMEM;
	}

	*ppsNewDir = psNewDir;
	psNewDir->ui32RefCount = 1;

	/* if parent directory is not gpsPVRDebugFSEntryDir, increment its refCount */
	if (psNewDir->psParentDir)
	{
		/* if we fail to acquire the reference that probably means that
		 * parent dir was already freed - we have to cleanup in this situation */
		if (!_RefDirEntry(psNewDir->psParentDir))
		{
			_UnrefAndMaybeDestroyDirEntry(ppsNewDir);
			return -EFAULT;
		}
	}

	return 0;
}

/*************************************************************************/ /*!
@Function		PVRDebugFSRemoveEntryDir
@Description	Remove a directory that was created by
				PVRDebugFSCreateEntryDir(). Any directories or files created
				under the directory being removed should be removed first.
@Input          ppsDir       Pointer representing the directory to be removed.
                             Has to be double pointer to avoid possible races
                             and use-after-free situations.
@Return			void
*/ /**************************************************************************/
void PVRDebugFSRemoveEntryDir(PVR_DEBUGFS_DIR_DATA **ppsDir)
{
	_UnrefAndMaybeDestroyDirEntry(ppsDir);
}

/*************************************************************************/ /*!
@Function		PVRDebugFSCreateEntry
@Description	Create an entry in the specified directory.
@Input			pszName			String containing the name for the entry.
@Input			psParentDir		Pointer from PVRDebugFSCreateEntryDir()
								representing the directory in which to create
								the entry or NULL for the root directory.
@Input			psReadOps		Pointer to structure containing the necessary
								functions to read from the entry.
@Input			pfnWrite		Callback function used to write to the entry.
								This function must update the offset pointer
								before it returns.
@Input			pvData			Private data to be passed to the read
								functions, in the seq_file private member, and
								the write function callback.
@Output			ppsNewEntry		On success, points to the newly created entry.
@Return			int				On success, returns 0. Otherwise, returns an
								error code.
*/ /**************************************************************************/
int PVRDebugFSCreateEntry(const char *pszName,
			  PVR_DEBUGFS_DIR_DATA *psParentDir,
			  const struct seq_operations *psReadOps,
			  PVRSRV_ENTRY_WRITE_FUNC *pfnWrite,
			  PVRSRV_INC_FSENTRY_PVDATA_REFCNT_FN *pfnIncPvDataRefCnt,
			  PVRSRV_DEC_FSENTRY_PVDATA_REFCNT_FN *pfnDecPvDataRefCnt,
			  void *pvData,
			  PVR_DEBUGFS_ENTRY_DATA **ppsNewEntry)
{
	PVR_DEBUGFS_PRIV_DATA *psPrivData;
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry;
	struct dentry *psEntry;
	umode_t uiMode;

	PVR_ASSERT(gpsPVRDebugFSEntryDir != NULL);
	PVR_ASSERT(!((pfnIncPvDataRefCnt != NULL && pfnDecPvDataRefCnt == NULL) ||
	           (pfnIncPvDataRefCnt == NULL && pfnDecPvDataRefCnt != NULL)));

	psPrivData = OSAllocMemNoStats(sizeof(*psPrivData));
	if (psPrivData == NULL)
	{
		return -ENOMEM;
	}
	psDebugFSEntry = OSAllocMemNoStats(sizeof(*psDebugFSEntry));
	if (psDebugFSEntry == NULL)
	{
		OSFreeMemNoStats(psPrivData);
		return -ENOMEM;
	}

	psPrivData->psReadOps = psReadOps;
	psPrivData->pfnWrite = pfnWrite;
	psPrivData->pvData = (void*)pvData;
	psPrivData->pfIncPvDataRefCnt = pfnIncPvDataRefCnt;
	psPrivData->pfDecPvDataRefCnt = pfnDecPvDataRefCnt;
	psPrivData->bValid = IMG_TRUE;
	/* Store ptr to debugFSEntry in psPrivData, so a ref can be taken on it
	 * when the client opens a file */
	psPrivData->psDebugFSEntry = psDebugFSEntry;

	uiMode = S_IFREG;

	if (psReadOps != NULL)
	{
		uiMode |= S_IRUGO;
	}

	if (pfnWrite != NULL)
	{
		uiMode |= S_IWUSR;
	}

	psDebugFSEntry->psParentDir = psParentDir;
	psDebugFSEntry->ui32RefCount = 1;
	psDebugFSEntry->psStatData = (PVR_DEBUGFS_DRIVER_STAT*)pvData;

	if (psDebugFSEntry->psParentDir)
	{
		/* increment refCount of parent directory */
		if (!_RefDirEntry(psDebugFSEntry->psParentDir))
		{
			kfree(psDebugFSEntry);
			kfree(psPrivData);
			return -EFAULT;
		}
	}

	psEntry = debugfs_create_file(pszName,
				      uiMode,
				      (psParentDir != NULL) ? psParentDir->psDir : gpsPVRDebugFSEntryDir,
				      psPrivData,
				      &gsPVRDebugFSFileOps);
	if (IS_ERR(psEntry))
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create debugfs '%s' file",
			 __FUNCTION__, pszName));

		return PTR_ERR(psEntry);
	}

	psDebugFSEntry->psEntry = psEntry;
	*ppsNewEntry = (void*)psDebugFSEntry;

	return 0;
}

/*************************************************************************/ /*!
@Function		PVRDebugFSRemoveEntry
@Description	Removes an entry that was created by PVRDebugFSCreateEntry().
@Input          ppsDebugFSEntry  Pointer representing the entry to be removed.
                Has to be double pointer to avoid possible races
                and use-after-free situations.
@Return			void
*/ /**************************************************************************/
void PVRDebugFSRemoveEntry(PVR_DEBUGFS_ENTRY_DATA **ppsDebugFSEntry)
{
	_UnrefAndMaybeDestroyDebugFSEntry(ppsDebugFSEntry);
}

/*************************************************************************/ /*!
@Function		PVRDebugFSCreateStatisticEntry
@Description	Create a statistic entry in the specified directory.
@Input			pszName			String containing the name for the entry.
@Input			psDir			Pointer from PVRDebugFSCreateEntryDir()
								representing the directory in which to create
								the entry or NULL for the root directory.
@Input			pfnStatsPrint	A callback function used to print all the
								statistics when reading from the statistic
								entry.
@Input			pfnIncStatMemRefCount	A callback function used take a
										reference on the memory backing the
										statistic.
@Input			pfnDecStatMemRefCount	A callback function used drop a
										reference on the memory backing the
										statistic.
@Input			pvData			Private data to be passed to the provided
								callback function.

@Return			PVR_DEBUGFS_DRIVER_STAT*   On success, a pointer representing
										   the newly created statistic entry.
										   Otherwise, NULL.
*/ /**************************************************************************/
PVR_DEBUGFS_DRIVER_STAT *PVRDebugFSCreateStatisticEntry(const char *pszName,
					 PVR_DEBUGFS_DIR_DATA *psDir,
				     OS_STATS_PRINT_FUNC *pfnStatsPrint,
					 PVRSRV_INC_STAT_MEM_REFCOUNT_FUNC	*pfnIncStatMemRefCount,
					 PVRSRV_INC_STAT_MEM_REFCOUNT_FUNC	*pfnDecStatMemRefCount,
				     void *pvData)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData;
	PVR_DEBUGFS_ENTRY_DATA * psDebugFSEntry;

	int iResult;

	if (pszName == NULL || pfnStatsPrint == NULL)
	{
		return NULL;
	}
	if ((pfnIncStatMemRefCount != NULL || pfnDecStatMemRefCount != NULL) && pvData == NULL)
	{
		return NULL;
	}

	psStatData = OSAllocZMemNoStats(sizeof(*psStatData));
	if (psStatData == NULL)
	{
		return NULL;
	}

	psStatData->pvData = pvData;
	psStatData->pfnStatsPrint = pfnStatsPrint;
	psStatData->pfnIncStatMemRefCount = pfnIncStatMemRefCount;
	psStatData->pfnDecStatMemRefCount = pfnDecStatMemRefCount;
	psStatData->ui32RefCount = 1;

	iResult = PVRDebugFSCreateEntry(pszName,
					psDir,
					&gsDebugFSStatisticReadOps,
					NULL,
					(PVRSRV_INC_FSENTRY_PVDATA_REFCNT_FN *) _RefStatEntry,
					(PVRSRV_DEC_FSENTRY_PVDATA_REFCNT_FN *) _UnrefAndMaybeDestroyStatEntry,
					psStatData,
					&psDebugFSEntry);
	if (iResult != 0)
	{
		OSFreeMemNoStats(psStatData);
		return NULL;
	}
	psStatData->pvDebugFSEntry = (void*)psDebugFSEntry;

	if (pfnIncStatMemRefCount)
	{
		/* call function to take reference on the memory holding the stat */
		psStatData->pfnIncStatMemRefCount((void*)psStatData->pvData);
	}

	psDebugFSEntry->ui32RefCount = 1;

	return psStatData;
}

/*************************************************************************/ /*!
@Function		PVRDebugFSRemoveStatisticEntry
@Description	Removes a statistic entry that was created by
				PVRDebugFSCreateStatisticEntry().
@Input			psStatEntry  Pointer representing the statistic entry to be
							 removed.
@Return			void
*/ /**************************************************************************/
void PVRDebugFSRemoveStatisticEntry(PVR_DEBUGFS_DRIVER_STAT *psStatEntry)
{
	PVR_ASSERT(psStatEntry != NULL);
	/* drop reference on pvStatEntry*/
	_UnrefAndMaybeDestroyStatEntry(psStatEntry);
}

#if defined(PVRSRV_ENABLE_MEMTRACK_STATS_FILE)
static void *_DebugFSRawStatisticSeqStart(struct seq_file *psSeqFile,
                                          loff_t *puiPosition)
{
	PVR_DEBUGFS_RAW_DRIVER_STAT *psStatData =
	        (PVR_DEBUGFS_RAW_DRIVER_STAT *) psSeqFile->private;

	if (psStatData)
	{
		if (*puiPosition == 0)
		{
			return psStatData;
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is"
		        " NULL", __func__));
	}

	return NULL;
}

static void _DebugFSRawStatisticSeqStop(struct seq_file *psSeqFile,
                                        void *pvData)
{
	PVR_DEBUGFS_RAW_DRIVER_STAT *psStatData =
	        (PVR_DEBUGFS_RAW_DRIVER_STAT *) psSeqFile->private;

	if (!psStatData)
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is"
		        " NULL", __func__));
	}
}

static void *_DebugFSRawStatisticSeqNext(struct seq_file *psSeqFile,
                                         void *pvData,
                                         loff_t *puiPosition)
{
	PVR_DEBUGFS_RAW_DRIVER_STAT *psStatData =
	        (PVR_DEBUGFS_RAW_DRIVER_STAT *) psSeqFile->private;
	PVR_UNREFERENCED_PARAMETER(pvData);

	if (!psStatData)
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is"
		        " NULL", __func__));
	}

	return NULL;
}

static int _DebugFSRawStatisticSeqShow(struct seq_file *psSeqFile, void *pvData)
{
	PVR_DEBUGFS_RAW_DRIVER_STAT *psStatData =
	        (PVR_DEBUGFS_RAW_DRIVER_STAT *) pvData;

	if (psStatData != NULL)
	{
		psStatData->pfStatsPrint((void *) psSeqFile, NULL,
		                         _StatsSeqPrintf);
		return 0;
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called when psStatData is"
		        " NULL, returning -ENODATA(%d)", __FUNCTION__, -ENODATA));
	}

	return -ENODATA;
}

static struct seq_operations gsDebugFSRawStatisticReadOps =
{
	.start = _DebugFSRawStatisticSeqStart,
	.stop  = _DebugFSRawStatisticSeqStop,
	.next  = _DebugFSRawStatisticSeqNext,
	.show  = _DebugFSRawStatisticSeqShow,
};

PVR_DEBUGFS_RAW_DRIVER_STAT *PVRDebugFSCreateRawStatisticEntry(
                                             const IMG_CHAR *pszFileName,
                                             void *pvParentDir,
                                             OS_STATS_PRINT_FUNC *pfStatsPrint)
{
	PVR_DEBUGFS_RAW_DRIVER_STAT *psStatData;
	PVR_DEBUGFS_ENTRY_DATA *psDebugFsEntry;

	int iResult;

	if (pszFileName == NULL || pfStatsPrint == NULL)
	{
		return NULL;
	}

	psStatData = OSAllocZMemNoStats(sizeof(*psStatData));
	if (psStatData == NULL)
	{
		return NULL;
	}

	psStatData->pfStatsPrint = pfStatsPrint;

	PVR_ASSERT((pvParentDir == NULL));

	iResult = PVRDebugFSCreateEntry(pszFileName,
	                                pvParentDir,
	                                &gsDebugFSRawStatisticReadOps,
	                                NULL,
	                                NULL,
	                                NULL,
	                                psStatData,
	                                &psDebugFsEntry);
	if (iResult != 0)
	{
		OSFreeMemNoStats(psStatData);
		return NULL;
	}
	psStatData->pvDebugFsEntry = (void *) psDebugFsEntry;

	psDebugFsEntry->ui32RefCount = 1;

	return psStatData;
}

void PVRDebugFSRemoveRawStatisticEntry(PVR_DEBUGFS_RAW_DRIVER_STAT *psStatEntry)
{
	PVR_ASSERT(psStatEntry != NULL);

	PVRDebugFSRemoveEntry(&psStatEntry->pvDebugFsEntry);
	OSFreeMemNoStats(psStatEntry);
}
#endif

static IMG_BOOL _RefDirEntry(PVR_DEBUGFS_DIR_DATA *psDirEntry)
{
	IMG_BOOL bStatus = IMG_FALSE;

	PVR_ASSERT(psDirEntry != NULL && psDirEntry->psDir != NULL);

	mutex_lock(&gDebugFSLock);

	if (psDirEntry->ui32RefCount > 0)
	{
		/* Increment refCount */
		psDirEntry->ui32RefCount++;
		bStatus = IMG_TRUE;
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called to ref psDirEntry '%s'"
		        " when ui32RefCount is zero", __FUNCTION__,
		        psDirEntry->psDir->d_iname));
	}

	mutex_unlock(&gDebugFSLock);

	return bStatus;
}

static void _UnrefAndMaybeDestroyDirEntryWhileLocked(PVR_DEBUGFS_DIR_DATA **ppsDirEntry)
{
	PVR_DEBUGFS_DIR_DATA *psDirEntry = *ppsDirEntry;

	PVR_ASSERT(psDirEntry != NULL && psDirEntry->psDir != NULL);

	if (psDirEntry->ui32RefCount > 0)
	{
		/* Decrement refCount and free if now zero */
		if (--psDirEntry->ui32RefCount == 0)
		{
			/* if parent directory is not gpsPVRDebugFSEntryDir, decrement its refCount */
			debugfs_remove(psDirEntry->psDir);
			if (psDirEntry->psParentDir)
			{
				_UnrefAndMaybeDestroyDirEntryWhileLocked(&psDirEntry->psParentDir);
			}
			OSFreeMemNoStats(psDirEntry);
			*ppsDirEntry = NULL;
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called to unref psDirEntry '%s'"
		        " when ui32RefCount is zero", __FUNCTION__,
		        psDirEntry->psDir->d_iname));
	}
}

static inline void _UnrefAndMaybeDestroyDirEntry(PVR_DEBUGFS_DIR_DATA **ppsDirEntry)
{
	mutex_lock(&gDebugFSLock);
	_UnrefAndMaybeDestroyDirEntryWhileLocked(ppsDirEntry);
	mutex_unlock(&gDebugFSLock);
}

static IMG_BOOL _RefDebugFSEntryNoLock(PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry)
{
	IMG_BOOL bResult = IMG_FALSE;

	PVR_ASSERT(psDebugFSEntry != NULL);

	bResult = (psDebugFSEntry->ui32RefCount > 0);
	if (bResult)
	{
		/* Increment refCount of psDebugFSEntry */
		psDebugFSEntry->ui32RefCount++;
	}

	return bResult;
}

static void _UnrefAndMaybeDestroyDebugFSEntry(PVR_DEBUGFS_ENTRY_DATA **ppsDebugFSEntry)
{
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry;

	mutex_lock(&gDebugFSLock);
	/* Decrement refCount of psDebugFSEntry, and free if now zero */
	psDebugFSEntry = *ppsDebugFSEntry;
	PVR_ASSERT(psDebugFSEntry != NULL);

	if (psDebugFSEntry->ui32RefCount > 0)
	{
		if (--psDebugFSEntry->ui32RefCount == 0)
		{
			struct dentry *psEntry = psDebugFSEntry->psEntry;

			if (psEntry)
			{
				/* Free any private data that was provided to debugfs_create_file() */
				if (psEntry->d_inode->i_private != NULL)
				{
					PVR_DEBUGFS_PRIV_DATA *psPrivData = (PVR_DEBUGFS_PRIV_DATA*)psDebugFSEntry->psEntry->d_inode->i_private;

					psPrivData->bValid = IMG_FALSE;
					psPrivData->psDebugFSEntry = NULL;
					OSFreeMemNoStats(psEntry->d_inode->i_private);
					psEntry->d_inode->i_private = NULL;
				}
				debugfs_remove(psEntry);
			}
			/* decrement refcount of parent directory */
			if (psDebugFSEntry->psParentDir)
			{
				_UnrefAndMaybeDestroyDirEntryWhileLocked(&psDebugFSEntry->psParentDir);
			}

			/* now free the memory allocated for psDebugFSEntry */
			OSFreeMemNoStats(psDebugFSEntry);
			*ppsDebugFSEntry = NULL;
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called to unref psDebugFSEntry '%s' when ui32RefCount is zero", __FUNCTION__, psDebugFSEntry->psEntry->d_iname));
	}

	mutex_unlock(&gDebugFSLock);
}

static IMG_BOOL _RefStatEntry(PVR_DEBUGFS_DRIVER_STAT *psStatEntry)
{
	IMG_BOOL bResult = IMG_FALSE;

	PVR_ASSERT(psStatEntry != NULL);

	mutex_lock(&gDebugFSLock);

	bResult = (psStatEntry->ui32RefCount > 0);
	if (bResult)
	{
		/* Increment refCount of psStatEntry */
		psStatEntry->ui32RefCount++;
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called to ref psStatEntry '%s' when ui32RefCount is zero", __FUNCTION__, psStatEntry->pvDebugFSEntry->psEntry->d_iname));
	}

	mutex_unlock(&gDebugFSLock);

	return bResult;
}

static IMG_BOOL _UnrefAndMaybeDestroyStatEntry(PVR_DEBUGFS_DRIVER_STAT *psStatEntry)
{
	IMG_BOOL bResult;

	PVR_ASSERT(psStatEntry != NULL);

	mutex_lock(&gDebugFSLock);

	bResult = (psStatEntry->ui32RefCount > 0);

	if (bResult)
	{
		/* Decrement refCount of psStatData, and free if now zero */
		if (--psStatEntry->ui32RefCount == 0)
		{
			mutex_unlock(&gDebugFSLock);

			if (psStatEntry->pvDebugFSEntry)
			{
				_UnrefAndMaybeDestroyDebugFSEntry((PVR_DEBUGFS_ENTRY_DATA**)&psStatEntry->pvDebugFSEntry);
			}
			if (psStatEntry->pfnDecStatMemRefCount)
			{
				/* call function to drop reference on the memory holding the stat */
				psStatEntry->pfnDecStatMemRefCount((void*)psStatEntry->pvData);
			}
			OSFreeMemNoStats(psStatEntry);
		}
		else
		{
			mutex_unlock(&gDebugFSLock);
		}
	}
	else
	{
		PVR_DPF((PVR_DEBUGFS_PVR_DPF_LEVEL, "%s: Called to unref psStatEntry '%s' when ui32RefCount is zero", __FUNCTION__, psStatEntry->pvDebugFSEntry->psEntry->d_iname));
		mutex_unlock(&gDebugFSLock);
	}

	return bResult;
}

int PVRDebugFSCreateBlobEntry(const char *pszName,
			  PVR_DEBUGFS_DIR_DATA *psParentDir,
			  void *pvData,
			  unsigned long size,
			  PVR_DEBUGFS_BLOB_ENTRY_DATA **ppsNewEntry)
{
	PVR_DEBUGFS_BLOB_ENTRY_DATA *psDebugFSEntry;
	struct dentry *psEntry;
	umode_t uiMode;

	PVR_ASSERT(gpsPVRDebugFSEntryDir != NULL);

	psDebugFSEntry = OSAllocMemNoStats(sizeof(*psDebugFSEntry));
	if (psDebugFSEntry == NULL)
	{
		return -ENOMEM;
	}

	uiMode = S_IFREG | S_IRUGO;

	psDebugFSEntry->psParentDir = psParentDir;
	psDebugFSEntry->blob.data = pvData;
	psDebugFSEntry->blob.size = size;

	if (psDebugFSEntry->psParentDir)
	{
		/* increment refCount of parent directory */
		if (!_RefDirEntry(psDebugFSEntry->psParentDir))
		{
			OSFreeMemNoStats(psDebugFSEntry);
			return -EFAULT;
		}
	}

	psEntry = debugfs_create_blob(pszName, 
			                      uiMode,
								  (psParentDir != NULL) ? psParentDir->psDir : gpsPVRDebugFSEntryDir,
								  &psDebugFSEntry->blob);
	if (IS_ERR(psEntry))
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create debugfs '%s' blob file",
			 __FUNCTION__, pszName));

		OSFreeMemNoStats(psDebugFSEntry);
		return PTR_ERR(psEntry);
	}

	psDebugFSEntry->psEntry = psEntry;
	*ppsNewEntry = (void*)psDebugFSEntry;

	return 0;
}

void PVRDebugFSRemoveBlobEntry(PVR_DEBUGFS_BLOB_ENTRY_DATA **ppsDebugFSEntry)
{
	PVR_DEBUGFS_BLOB_ENTRY_DATA *psDebugFSEntry;

	PVR_ASSERT(ppsDebugFSEntry != NULL);

	psDebugFSEntry = *ppsDebugFSEntry;
	PVR_ASSERT(psDebugFSEntry != NULL);

	mutex_lock(&gDebugFSLock);

	if (psDebugFSEntry->psEntry)
	{
		debugfs_remove(psDebugFSEntry->psEntry);
	}

	/* decrement refcount of parent directory */
	if (psDebugFSEntry->psParentDir)
	{
		_UnrefAndMaybeDestroyDirEntryWhileLocked(&psDebugFSEntry->psParentDir);
	}

	/* now free the memory allocated for psDebugFSEntry */
	OSFreeMemNoStats(psDebugFSEntry);
	*ppsDebugFSEntry = NULL;

	mutex_unlock(&gDebugFSLock);
}
