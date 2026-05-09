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
 * Export two distinct versions of dlsym, to cover all GLIBC versions.
 */

NVSHARE_PROC_ADDR {
    global:
        cuGetProcAddress;
        cuGetProcAddress_v2;
};

NVSHARE_VMM {
    global:
        cuMemCreate;
        cuMemAddressReserve;
        cuMemMap;
        cuMemSetAccess;
        cuMemUnmap;
        cuMemRelease;
        nvshare_get_vmm_total_bytes;
};

NVSHARE_ASYNC_ALLOC {
    global:
        cuMemAllocAsync;
        cuMemFreeAsync;
        cuMemAllocFromPoolAsync;
        cuMemPoolCreate;
        cuMemPoolDestroy;
        nvshare_get_async_alloc_total;
};

NVSHARE_UM_HINTS {
    global:
        cuMemPrefetchAsync;
        cuMemAdvise;
        cuMemRangeGetAttribute;
};

GLIBC_2.2.5 {
    global:
        dlsym;
};

GLIBC_2.34 {
    global:
        dlsym;
} GLIBC_2.2.5;
