/*
 * Copyright (c) 2023 Georgios Alexopoulos
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Single declarative table of every CUDA symbol nvshare intercepts.
 *
 * Adding a new hook requires exactly one new line here — no more silent
 * bypass risk when the CUDA Runtime calls cuGetProcAddress.
 *
 * NOTE: cuda_defs.h maps several bare names to their _v2 variants via
 * #define (e.g. cuMemAlloc → cuMemAlloc_v2).  After preprocessing, every
 * extern declaration and every (void*) cast below therefore refers to the
 * _v2 symbol.  The table still carries *both* the bare name and the _v2
 * name as string keys so that callers asking for either form are covered.
 */

#include <stddef.h>
#include <string.h>

#include "hook_proc_addr.h"

/* -------------------------------------------------------------------------
 * Forward declarations for all hooked functions.
 *
 * cuda_defs.h #defines the bare names to _v2 variants, so these externs
 * resolve to the versioned symbols that are actually linked.
 * -------------------------------------------------------------------------*/

/* hook.c — core hooks */
extern CUresult cuMemAlloc(CUdeviceptr *dptr, size_t bytesize);
extern CUresult cuMemFree(CUdeviceptr dptr);
extern CUresult cuMemGetInfo(size_t *free, size_t *total);
extern CUresult cuInit(unsigned int flags);
extern CUresult cuLaunchKernel(CUfunction f,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream,
    void **kernelParams, void **extra);
extern CUresult cuMemcpy(CUdeviceptr dst, CUdeviceptr src, size_t ByteCount);
extern CUresult cuMemcpyAsync(CUdeviceptr dst, CUdeviceptr src,
    size_t ByteCount, CUstream hStream);
extern CUresult cuMemcpyDtoH(void *dstHost, CUdeviceptr srcDevice,
    size_t ByteCount);
extern CUresult cuMemcpyDtoHAsync(void *dstHost, CUdeviceptr srcDevice,
    size_t ByteCount, CUstream hStream);
extern CUresult cuMemcpyHtoD(CUdeviceptr dstDevice, const void *srcHost,
    size_t ByteCount);
extern CUresult cuMemcpyHtoDAsync(CUdeviceptr dstDevice, const void *srcHost,
    size_t ByteCount, CUstream hStream);
extern CUresult cuMemcpyDtoD(CUdeviceptr dstDevice, CUdeviceptr srcDevice,
    size_t ByteCount);
extern CUresult cuMemcpyDtoDAsync(CUdeviceptr dstDevice, CUdeviceptr srcDevice,
    size_t ByteCount, CUstream hStream);
extern CUresult cuGetProcAddress(const char *symbol, void **pfn,
    int cudaVersion, cuuint64_t flags);
extern CUresult cuGetProcAddress_v2(const char *symbol, void **pfn,
    int cudaVersion, cuuint64_t flags,
    CUdriverProcAddressQueryResult *symbolStatus);

/* hook_vmm.c — Virtual Memory Management family (#1) */
extern CUresult cuMemCreate(CUmemGenericAllocationHandle *handle, size_t size,
    const CUmemAllocationProp *prop, unsigned long long flags);
extern CUresult cuMemAddressReserve(CUdeviceptr *ptr, size_t size,
    size_t alignment, CUdeviceptr addr, unsigned long long flags);
extern CUresult cuMemMap(CUdeviceptr ptr, size_t size, size_t offset,
    CUmemGenericAllocationHandle handle, unsigned long long flags);
extern CUresult cuMemSetAccess(CUdeviceptr ptr, size_t size,
    const CUmemAccessDesc *desc, size_t count);
extern CUresult cuMemUnmap(CUdeviceptr ptr, size_t size);
extern CUresult cuMemRelease(CUmemGenericAllocationHandle handle);

/* hook_async_alloc.c — stream-ordered allocator family (#2) */
extern CUresult cuMemAllocAsync(CUdeviceptr *dptr, size_t bytesize,
    CUstream hStream);
extern CUresult cuMemFreeAsync(CUdeviceptr dptr, CUstream hStream);
extern CUresult cuMemAllocFromPoolAsync(CUdeviceptr *dptr, size_t bytesize,
    CUmemoryPool pool, CUstream hStream);
extern CUresult cuMemPoolCreate(CUmemoryPool *pool,
    const CUmemPoolProps *poolProps);
extern CUresult cuMemPoolDestroy(CUmemoryPool pool);

/* hook_um_hints.c — Unified Memory hint family (#3) */
extern CUresult cuMemPrefetchAsync(CUdeviceptr devPtr, size_t count,
    CUdevice dstDevice, CUstream hStream);
extern CUresult cuMemAdvise(CUdeviceptr devPtr, size_t count,
    int advice, CUdevice device);
extern CUresult cuMemRangeGetAttribute(void *data, size_t dataSize,
    int attribute, CUdeviceptr devPtr, size_t count);

/* -------------------------------------------------------------------------
 * Dispatch table.
 *
 * Each row maps a symbol name string to our interposed function pointer.
 * Both the canonical name and any _v2 alias that cuda.h #defines are listed
 * so callers asking for either form are hooked correctly.
 *
 * Keep the table static — nvshare_dispatch_proc_addr() and
 * nvshare_dispatch_table_size() are the only public surface.
 * -------------------------------------------------------------------------*/

static const struct dispatch_entry {
    const char *name;
    void       *fn;
} table[] = {
    /* core memory allocation */
    {"cuMemAlloc",                  (void *)cuMemAlloc},
    {"cuMemAlloc_v2",               (void *)cuMemAlloc},
    {"cuMemFree",                   (void *)cuMemFree},
    {"cuMemFree_v2",                (void *)cuMemFree},
    {"cuMemGetInfo",                (void *)cuMemGetInfo},
    {"cuMemGetInfo_v2",             (void *)cuMemGetInfo},

    /* driver initialisation */
    {"cuInit",                      (void *)cuInit},

    /* kernel launch */
    {"cuLaunchKernel",              (void *)cuLaunchKernel},

    /* memory copy — bare names */
    {"cuMemcpy",                    (void *)cuMemcpy},
    {"cuMemcpyAsync",               (void *)cuMemcpyAsync},
    {"cuMemcpyDtoH",                (void *)cuMemcpyDtoH},
    {"cuMemcpyDtoH_v2",             (void *)cuMemcpyDtoH},
    {"cuMemcpyDtoHAsync",           (void *)cuMemcpyDtoHAsync},
    {"cuMemcpyDtoHAsync_v2",        (void *)cuMemcpyDtoHAsync},
    {"cuMemcpyHtoD",                (void *)cuMemcpyHtoD},
    {"cuMemcpyHtoD_v2",             (void *)cuMemcpyHtoD},
    {"cuMemcpyHtoDAsync",           (void *)cuMemcpyHtoDAsync},
    {"cuMemcpyHtoDAsync_v2",        (void *)cuMemcpyHtoDAsync},
    {"cuMemcpyDtoD",                (void *)cuMemcpyDtoD},
    {"cuMemcpyDtoD_v2",             (void *)cuMemcpyDtoD},
    {"cuMemcpyDtoDAsync",           (void *)cuMemcpyDtoDAsync},
    {"cuMemcpyDtoDAsync_v2",        (void *)cuMemcpyDtoDAsync},

    /* proc-address hooks (self-referential; keeps dlsym path consistent) */
    {"cuGetProcAddress",            (void *)cuGetProcAddress},
    {"cuGetProcAddress_v2",         (void *)cuGetProcAddress_v2},

    /* VMM family — CUDA 10.2+ */
    {"cuMemCreate",                 (void *)cuMemCreate},
    {"cuMemAddressReserve",         (void *)cuMemAddressReserve},
    {"cuMemMap",                    (void *)cuMemMap},
    {"cuMemSetAccess",              (void *)cuMemSetAccess},
    {"cuMemUnmap",                  (void *)cuMemUnmap},
    {"cuMemRelease",                (void *)cuMemRelease},

    /* stream-ordered allocator — CUDA 11.2+ */
    {"cuMemAllocAsync",             (void *)cuMemAllocAsync},
    {"cuMemFreeAsync",              (void *)cuMemFreeAsync},
    {"cuMemAllocFromPoolAsync",     (void *)cuMemAllocFromPoolAsync},
    {"cuMemPoolCreate",             (void *)cuMemPoolCreate},
    {"cuMemPoolDestroy",            (void *)cuMemPoolDestroy},

    /* Unified Memory hints — CUDA 8+ */
    {"cuMemPrefetchAsync",          (void *)cuMemPrefetchAsync},
    {"cuMemAdvise",                 (void *)cuMemAdvise},
    {"cuMemRangeGetAttribute",      (void *)cuMemRangeGetAttribute},
};

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int nvshare_dispatch_proc_addr(const char *symbol, void **pfn)
{
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(symbol, table[i].name) == 0) {
            *pfn = table[i].fn;
            return 1;
        }
    }
    return 0;
}

int nvshare_dispatch_table_size(void)
{
    return (int)(sizeof(table) / sizeof(table[0]));
}
