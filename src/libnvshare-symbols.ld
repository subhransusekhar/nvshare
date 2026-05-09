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
 * Linker version script for libnvshare.so.
 *
 * The whole point of LD_PRELOAD interposition is that every CUDA Driver
 * API and NVML symbol we hook MUST appear in the dynamic symbol table
 * so the dynamic linker resolves the application's calls to *our*
 * implementations. Use globs to cover everything cu* / nvml* — adding
 * a new hook then never requires touching this file.
 *
 * dlsym is versioned to cover both the historical glibc (GLIBC_2.2.5)
 * and modern glibc (GLIBC_2.34) variants, since the dynamic loader
 * picks the version-tagged dlsym based on the caller's binding.
 *
 * Everything else (internal helpers, log emitters, proc-addr
 * dispatch, etc.) is hidden via the catch-all "local: *;" so the
 * .so's dynamic symbol surface stays small.
 */

NVSHARE_HOOKS {
    global:
        /* CUDA Driver API hooks: cuMemAlloc, cuLaunchKernel,
         * cuCtxSynchronize, cuMemcpy*, cuGetProcAddress*, the VMM
         * family (#1), the stream-ordered allocator family (#2),
         * the UM hint APIs (#3), etc. */
        cu*;

        /* NVML hooks. */
        nvml*;

        /* Introspection helpers exposed for telemetry / tests. */
        nvshare_get_*;
    local:
        *;
};

GLIBC_2.2.5 {
    global:
        dlsym;
};

GLIBC_2.34 {
    global:
        dlsym;
} GLIBC_2.2.5;
