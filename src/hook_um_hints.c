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
 * Hook CUDA Unified Memory hint function calls.
 *
 * cuMemPrefetchAsync, cuMemAdvise, and cuMemRangeGetAttribute are pass-through
 * hooks that ensure dispatch coverage on cuGetProcAddress so that clients
 * calling these APIs receive our shim rather than the raw libcuda pointer.
 *
 * M0 only intercepts and logs (prefetch and advise).  M2 will use the hints
 * in the warmup-on-lock-grant path.  cuMemRangeGetAttribute is intentionally
 * silent — it is called frequently by the CUDA driver itself and logging would
 * flood NV_DEBUG output.
 *
 * These APIs were added in CUDA 8 and are present on all supported platforms.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

#include "cuda_defs.h"
#include "common.h"

/* Real underlying function pointers — populated by init_real_funcs(). */
cuMemPrefetchAsync_func      real_cuMemPrefetchAsync      = NULL;
cuMemAdvise_func             real_cuMemAdvise             = NULL;
cuMemRangeGetAttribute_func  real_cuMemRangeGetAttribute  = NULL;

/*
 * Forward declaration — defined in hook.c.
 * Triggers the same pthread_once bootstrap that cuInit / cuGetProcAddress use,
 * which in turn calls bootstrap_cuda() and populates all real_* pointers.
 */
extern void init_real_funcs(void);

/* --------------------------------------------------------------------------
 * Hook implementations
 * -------------------------------------------------------------------------- */

CUresult cuMemPrefetchAsync(CUdeviceptr devPtr, size_t count,
                            CUdevice dstDevice, CUstream hStream)
{
	if (!real_cuMemPrefetchAsync) init_real_funcs();
	log_debug("hook: cuMemPrefetchAsync size=%zu dst=%d", count, dstDevice);
	return real_cuMemPrefetchAsync(devPtr, count, dstDevice, hStream);
}

CUresult cuMemAdvise(CUdeviceptr devPtr, size_t count,
                     int advice, CUdevice device)
{
	if (!real_cuMemAdvise) init_real_funcs();
	log_debug("hook: cuMemAdvise size=%zu advice=%d", count, advice);
	return real_cuMemAdvise(devPtr, count, advice, device);
}

/*
 * No log_debug here — cuMemRangeGetAttribute is called frequently by the
 * CUDA driver and logging every call would flood NV_DEBUG output.
 */
CUresult cuMemRangeGetAttribute(void *data, size_t dataSize,
                                int attribute, CUdeviceptr devPtr, size_t count)
{
	if (!real_cuMemRangeGetAttribute) init_real_funcs();
	return real_cuMemRangeGetAttribute(data, dataSize, attribute, devPtr, count);
}
