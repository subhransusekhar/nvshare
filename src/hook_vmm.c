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
 *
 * Hook CUDA Virtual Memory Management (VMM) function calls.
 *
 * Modern PyTorch (>= 2.1) caching allocator uses cuMemCreate + cuMemMap
 * ("expandable_segments") instead of cuMemAlloc.  Without these hooks,
 * nvshare cannot observe the working set of such workloads.
 *
 * We do NOT convert VMM allocations to managed memory — VMM is a different
 * abstraction (handle + reserve + map) and the conversion would be
 * meaningless.  We pass all calls through, track physical-memory handles in a
 * local list so we can account for outstanding bytes, and emit NV_DEBUG lines.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

#include <stdlib.h>
#include <pthread.h>

#include "cuda_defs.h"
#include "common.h"

/* Real underlying function pointers — populated by init_real_funcs(). */
cuMemCreate_func          real_cuMemCreate          = NULL;
cuMemAddressReserve_func  real_cuMemAddressReserve  = NULL;
cuMemMap_func             real_cuMemMap             = NULL;
cuMemSetAccess_func       real_cuMemSetAccess       = NULL;
cuMemUnmap_func           real_cuMemUnmap           = NULL;
cuMemRelease_func         real_cuMemRelease         = NULL;

/*
 * Forward declaration — defined in hook.c.
 * Triggers the same pthread_once bootstrap that cuInit / cuGetProcAddress use,
 * which in turn calls bootstrap_cuda() and populates all real_* pointers.
 */
extern void init_real_funcs(void);

/* --------------------------------------------------------------------------
 * Local accounting list — minimal; just enough to count outstanding bytes.
 * We use a dedicated mutex (vmm_mu) to avoid holding kcount_mutex on
 * allocation paths (see anti-pattern list in issue #1).
 * -------------------------------------------------------------------------- */
struct vmm_alloc {
	CUmemGenericAllocationHandle h;
	size_t size;
	struct vmm_alloc *next;
};

static struct vmm_alloc *vmm_list        = NULL;
static pthread_mutex_t   vmm_mu          = PTHREAD_MUTEX_INITIALIZER;
static size_t            vmm_total_bytes = 0;

/* --------------------------------------------------------------------------
 * Hook implementations
 * -------------------------------------------------------------------------- */

CUresult cuMemCreate(CUmemGenericAllocationHandle *handle, size_t size,
                     const CUmemAllocationProp *prop, unsigned long long flags)
{
	if (!real_cuMemCreate) init_real_funcs();
	CUresult rc = real_cuMemCreate(handle, size, prop, flags);
	if (rc == CUDA_SUCCESS) {
		struct vmm_alloc *a = malloc(sizeof(*a));
		if (a) {
			a->h    = *handle;
			a->size = size;
			pthread_mutex_lock(&vmm_mu);
			a->next  = vmm_list;
			vmm_list = a;
			vmm_total_bytes += size;
			pthread_mutex_unlock(&vmm_mu);
		}
		log_debug("hook: cuMemCreate size=%zu total_vmm=%zu",
		          size, vmm_total_bytes);
	}
	return rc;
}

CUresult cuMemRelease(CUmemGenericAllocationHandle handle)
{
	if (!real_cuMemRelease) init_real_funcs();
	pthread_mutex_lock(&vmm_mu);
	struct vmm_alloc **pp = &vmm_list;
	while (*pp) {
		if ((*pp)->h == handle) {
			struct vmm_alloc *gone = *pp;
			*pp = gone->next;
			vmm_total_bytes -= gone->size;
			free(gone);
			break;
		}
		pp = &(*pp)->next;
	}
	pthread_mutex_unlock(&vmm_mu);
	return real_cuMemRelease(handle);
}

/*
 * Address-space and mapping operations: pass-through with NV_DEBUG only.
 * They don't allocate physical memory — cuMemCreate already did that.
 */
CUresult cuMemAddressReserve(CUdeviceptr *ptr, size_t size, size_t alignment,
                             CUdeviceptr addr, unsigned long long flags)
{
	if (!real_cuMemAddressReserve) init_real_funcs();
	CUresult rc = real_cuMemAddressReserve(ptr, size, alignment, addr, flags);
	log_debug("hook: cuMemAddressReserve size=%zu rc=%d", size, (int)rc);
	return rc;
}

CUresult cuMemMap(CUdeviceptr ptr, size_t size, size_t offset,
                  CUmemGenericAllocationHandle handle, unsigned long long flags)
{
	if (!real_cuMemMap) init_real_funcs();
	CUresult rc = real_cuMemMap(ptr, size, offset, handle, flags);
	log_debug("hook: cuMemMap size=%zu rc=%d", size, (int)rc);
	return rc;
}

CUresult cuMemSetAccess(CUdeviceptr ptr, size_t size,
                        const CUmemAccessDesc *desc, size_t count)
{
	if (!real_cuMemSetAccess) init_real_funcs();
	CUresult rc = real_cuMemSetAccess(ptr, size, desc, count);
	log_debug("hook: cuMemSetAccess size=%zu count=%zu rc=%d",
	          size, count, (int)rc);
	return rc;
}

CUresult cuMemUnmap(CUdeviceptr ptr, size_t size)
{
	if (!real_cuMemUnmap) init_real_funcs();
	CUresult rc = real_cuMemUnmap(ptr, size);
	log_debug("hook: cuMemUnmap size=%zu rc=%d", size, (int)rc);
	return rc;
}

/* Exported so other TUs can read the counter for telemetry. */
size_t nvshare_get_vmm_total_bytes(void)
{
	size_t v;
	pthread_mutex_lock(&vmm_mu);
	v = vmm_total_bytes;
	pthread_mutex_unlock(&vmm_mu);
	return v;
}
