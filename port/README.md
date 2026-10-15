# port/

The loader side of Shadow Guardian Bridge, built for armhf and run under qemu-user.

| path | what |
|---|---|
| `game/` | the Shadow Guardian layer: `main.cpp` (boot order and frame loop), `io_paths.cpp` (path rules), `zip_mount.cpp` (cache zip mounted as the data folder), `input_bridge.cpp` (touch, Back), `jni/` (fake Java classes, `AudioTrack`) |
| `portbase/` | the base: bionic ELF loader, libc thunks, fake JVM, GLES bridge (from the N.O.V.A. 3 port; `docs/PORT-CONTRACT.md`) |
| `android/` | the APK wrapper (`build_apk.sh`, Java sources, manifest) — see `android/README.md` |
| `tools/` | dependency collection and the glibc floor check |

Build from the repository root: `./build.sh` -> `build/shadowguardian`. Usage: `shadowguardian <game-directory>`, where
the directory holds `lib/armeabi-v7a/libshadowguardian.so`, `assets/`, `res/` from the game's APK and the cache
(`GloftSGHP.zip` or an unpacked `GloftSGHP/`). Runtime switches are environment variables with the `SG_` prefix.
