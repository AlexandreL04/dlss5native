# Credits and third-party notices

dlss5native contains no NVIDIA binary and no NVIDIA model. It calls files you already own.

## RenoDX — DLSS Neural Rendering add-on

The colour stage (`src/shaders.h`, `src/colorstage.cpp`) follows the algorithms of the RenoDX DLSS
Neural Rendering add-on, v8.5: the Display and divisor codecs, the one-sided divisor governor, the
exposure feed and its guard, the pedestal removal, the detail-stability temporal filter, the edge-aware
transport for a reduced working resolution, and the frame classification. They were re-implemented for
this project's own pipeline; the constants and the mathematics are RenoDX's.

The way the model is called without Streamline also follows that add-on: the caller-name answer given
to the model (`src/ngx.cpp`) and the application id it is initialized with.

- Add-on source: https://github.com/Andarael/RenoDX-DLSS5-Generic (branch `v8.5dev`)
- RenoDX: https://github.com/clshortfuse/renodx
- MIT License, Copyright (c) 2025 Carlos Lopez Jr.

```
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Microsoft Detours

Function hooking. Built from source as a dependency (`deps/Detours`), not modified.

- https://github.com/microsoft/Detours
- MIT License, Copyright (c) Microsoft Corporation.

## NVIDIA NGX SDK headers

Public headers only (`nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h`), used at build
time from a checkout of the Streamline SDK (`deps/Streamline`). Not redistributed here.

- https://github.com/NVIDIA-RTX/Streamline
- Streamline: MIT License, Copyright (c) NVIDIA Corporation. NGX SDK headers: NVIDIA RTX SDKs License.

NVIDIA, DLSS, RTX and Streamline are trademarks of NVIDIA Corporation. This project is not affiliated
with, endorsed by, or supported by NVIDIA.
