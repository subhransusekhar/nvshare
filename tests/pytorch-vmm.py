# Copyright (c) 2023 Georgios Alexopoulos
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
# Force PyTorch to use the expandable-segments path, which uses cuMemCreate/cuMemMap.
# This is a fallback for direct `python tests/pytorch-vmm.py` invocations;
# the canonical source is the PYTORCH_CUDA_ALLOC_CONF env var in the K8s manifest.
os.environ.setdefault("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True")
import torch
import time

def main():
    assert torch.cuda.is_available(), "CUDA not available"
    dev = torch.device("cuda:0")
    print("torch", torch.__version__, "cuda", torch.version.cuda, flush=True)

    # Allocate a few large tensors so the allocator must back them with VMM regions.
    tensors = []
    size_mb = 512
    for i in range(8):
        t = torch.empty((size_mb * 1024 * 1024 // 4,), dtype=torch.float32, device=dev)
        t.fill_(float(i))
        tensors.append(t)
        print(f"allocated tensor {i}: {size_mb}MB; total~{(i+1)*size_mb}MB", flush=True)

    # Run a hot loop so the scheduler sees lock requests.
    for step in range(1000):
        s = sum(t.sum() for t in tensors)
        if step % 50 == 0:
            print(f"step={step} sum={float(s):.0f}", flush=True)
            time.sleep(0.05)

    print("Test PASSED", flush=True)

if __name__ == "__main__":
    main()
