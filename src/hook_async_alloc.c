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
 * Hook CUDA Stream-Ordered Allocator function calls.
 *
 * PyTorch's cudaMallocAsync backend (default on CUDA 11.2+) and TensorFlow's
 * stream-ordered allocations call cuMemAllocAsync / cuMemAllocFromPoolAsync
 * rather than the synchronous cuMemAlloc.  Without these hooks nvshare cannot
 * observe the working set of such workloads.
 *
 * We do NOT convert async allocations to managed memory — async allocator
 * semantics are bound to streams and pools, and substituting would change
 * observable behaviour.  We pass all calls through, maintain an atomic byte
 * counter for the two alloc paths, and emit log_debug lines.
 *
 * cuMemFreeAsync is intentionally not tracked: the size is unknown at free
 * time without maintaining internal pool state (see Anti-patterns in issue #2).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

#include <stdatomic.h>

#include "cuda_defs.h"
#include "common.h"

/* Real underlying function pointers — populated by init_real_funcs(). */
cuMemAllocAsync_func          real_cuMemAllocAsync          = NULL;
cuMemFreeAsync_func           real_cuMemFreeAsync           = NULL;
cuMemAllocFromPoolAsync_func  real_cuMemAllocFromPoolAsync  = NULL;
cuMemPoolCreate_func          real_cuMemPoolCreate          = NULL;
cuMemPoolDestroy_func         real_cuMemPoolDestroy         = NULL;

/*
 * Forward declaration — defined in hook.c.
 * Triggers the same pthread_once bootstrap that cuInit / cuGetProcAddress use,
 * which in turn calls bootstrap_cuda() and populates all real_* pointers.
 */
extern void init_real_funcs(void);

/*
 * Running total of bytes requested via the async-alloc paths.
 * We use an atomic rather than a mutex: updates are single add/load ops and
 * we never need a consistent snapshot across multiple fields.
 */
static _Atomic size_t async_alloc_total = 0;

/* --------------------------------------------------------------------------
 * Hook implementations
 * -------------------------------------------------------------------------- */

CUresult cuMemAllocAsync(CUdeviceptr *dptr, size_t bytesize, CUstream hStream)
{
	if (!real_cuMemAllocAsync) init_real_funcs();
	CUresult rc = real_cuMemAllocAsync(dptr, bytesize, hStream);
	if (rc == CUDA_SUCCESS) {
		atomic_fetch_add(&async_alloc_total, bytesize);
		log_debug("hook: cuMemAllocAsync size=%zu total_async=%zu",
		          bytesize, atomic_load(&async_alloc_total));
	}
	return rc;
}

/*
 * Size is unknown at free time — the async pool tracks it internally.
 * Pass through without updating the counter (see Anti-patterns in issue #2).
 */
CUresult cuMemFreeAsync(CUdeviceptr dptr, CUstream hStream)
{
	if (!real_cuMemFreeAsync) init_real_funcs();
	return real_cuMemFreeAsync(dptr, hStream);
}

CUresult cuMemAllocFromPoolAsync(CUdeviceptr *dptr, size_t bytesize,
                                 CUmemoryPool pool, CUstream hStream)
{
	if (!real_cuMemAllocFromPoolAsync) init_real_funcs();
	CUresult rc = real_cuMemAllocFromPoolAsync(dptr, bytesize, pool, hStream);
	if (rc == CUDA_SUCCESS) {
		atomic_fetch_add(&async_alloc_total, bytesize);
		log_debug("hook: cuMemAllocFromPoolAsync size=%zu total_async=%zu",
		          bytesize, atomic_load(&async_alloc_total));
	}
	return rc;
}

/* Pool lifecycle — pass-through only; no memory accounting needed here. */
CUresult cuMemPoolCreate(CUmemoryPool *pool, const CUmemPoolProps *poolProps)
{
	if (!real_cuMemPoolCreate) init_real_funcs();
	CUresult rc = real_cuMemPoolCreate(pool, poolProps);
	log_debug("hook: cuMemPoolCreate rc=%d", (int)rc);
	return rc;
}

CUresult cuMemPoolDestroy(CUmemoryPool pool)
{
	if (!real_cuMemPoolDestroy) init_real_funcs();
	CUresult rc = real_cuMemPoolDestroy(pool);
	log_debug("hook: cuMemPoolDestroy rc=%d", (int)rc);
	return rc;
}

/* Exported so other TUs can read the counter for telemetry. */
size_t nvshare_get_async_alloc_total(void)
{
	return atomic_load(&async_alloc_total);
}
