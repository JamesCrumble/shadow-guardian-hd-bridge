# Third-party code and licensing

This port is released under **GPL-3.0** (see `LICENSE`). It carries code from
several upstream projects; this file records where each part came from, because
the licences differ and the originals must keep their attribution.

This repository is Shadow Guardian Bridge. Its base - the loader, thunks and
fake JVM (`portbase/`) - comes from the N.O.V.A. 3 PortMaster port, whose
project direction and `eapx` extraction tool were created by **EapRules**.

The Android bridge in this repository - the APK wrapper (`android/`), the GLES bridge to the phone's GPU
(`portbase/src/gl_bridge*`), the qemu patches (`../qemu/`) and the phone scripts (`../tools/`) - was written
by **JamesCrumble** on top of that port.

## Where the code comes from

| Part | Origin | Licence |
|---|---|---|
| `portbase/loader/` — bionic ELF loader, relocations, symbol hooking | [gmloader-next](https://github.com/JohnnyonFlame/gmloader-next) by JohnnyonFlame, itself derived from the Vita so-loader by **Andy Nguyen** | GPL (see note) |
| `portbase/thunks/libc/` — bionic→glibc thunks | gmloader-next | GPL |
| `portbase/thunks/libc/time64.cpp` | `y2038` by **Michael G Schwern** | MIT / Artistic |
| `portbase/thunks/libc/fortify.cpp` | The Android Open Source Project (parts © Regents of the University of California) | Apache-2.0 / BSD |
| `portbase/jni/jni.h` | The Android Open Source Project | Apache-2.0 |
| `portbase/loader/leb128.h` | Free Software Foundation (binutils) | GPL |
| `portbase/thunks/khronos/` | glad generator, Khronos headers | MIT / Apache-2.0 |
| `portbase/third_party/powervr/PVRTDecompress.*` | [PowerVR SDK](https://github.com/powervr-graphics/Native_SDK) by Imagination Technologies | MIT |
| `portbase/src/vfp_vector_patch.cpp` register decoding/scalar emission | adapted from VFPVector by **Bythos14** | MIT |
| SWP/SWPB emulation in the SIGILL handler | written for this port family; the technique follows the Vita so-loader lineage | GPL-3.0 |
| `tools/eapx.py` — transactional first-boot donor extractor | written by **EapRules** | GPL-3.0 |
| `game/` (Shadow Guardian layer: entry points, JNI classes, cache-zip mount) | written by **JamesCrumble** | GPL-3.0 |
| `android/`, `portbase/src/gl_bridge*`, `../qemu/`, `../tools/` | Android bridge by **JamesCrumble** | GPL-3.0 (qemu patches: GPL-2.0-or-later, as QEMU) |

The game itself — `libshadowguardian.so`, the GloftSGHP cache and every asset —
is © Gameloft and is **never** distributed with this port: the user supplies
their own legitimately obtained copy (BYOG).
