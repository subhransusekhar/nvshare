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
 * Single-source-of-truth dispatch table for hooked CUDA symbols.
 */

#ifndef NVSHARE_HOOK_PROC_ADDR_H
#define NVSHARE_HOOK_PROC_ADDR_H

#include "cuda_defs.h"

/*
 * If symbol is hooked, overwrite *pfn with our interposed function pointer
 * and return 1.  If symbol is not in the table, leave *pfn unchanged and
 * return 0.
 */
int nvshare_dispatch_proc_addr(const char *symbol, void **pfn);

/* Returns the number of entries in the dispatch table (for CI / testing). */
int nvshare_dispatch_table_size(void);

#endif /* NVSHARE_HOOK_PROC_ADDR_H */
