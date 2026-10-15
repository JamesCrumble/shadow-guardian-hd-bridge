# Shadow Guardian Bridge — APK wrapper

The Android side: a Java app (no NDK) that runs the port under our `qemu-arm` and replays its GLES calls on the
phone's GPU. The structure is the one of NOVA3 Bridge; what is specific to this game is listed here.

| file | what |
|---|---|
| `src/.../GameProcess.java` | unpacks the sysroot, takes `lib/`, `assets/`, `res/{drawable,raw}/` out of the game's APK (once per APK), passes the cache zip as it is (`SG_CACHE_ZIP`) or links an unpacked `GloftSGHP/`, starts qemu, feeds the game's PCM to an `AudioTrack` |
| `src/.../MainActivity.java` | the surface in a centred 5:3 stage (black bars; `stretch.flag` fills the screen), touches -> `touch a x y id`, Back -> `key 4` |
| `src/.../GlBridge.java`, `GlOps.java` | the GLES bridge host side; `GlOps.java` and `../portbase/src/gl_bridge_gen.inc` are written by `tools/gen_glbridge.py` |
| `build_apk.sh` | builds `ShadowGuardian-Bridge-debug.apk`; `RELEASE=1` signs with `build/release.keystore` |

On the phone everything lives in `Android/data/com.jamescrumble.shadowguardian/files/`: the game's APK, the cache zip,
`sg.log`, and the settings files (`engine_env.txt`, `resolution.txt`, `stretch.flag`, `notrace.flag`, `qemu_args.txt`).

Device constraints (seccomp, exec only from the native library dir, qemu's silent load errors) and how the bridge
handles them are described in NOVA3 Bridge's `port/android/README.md` and porting guide; nothing differs here.
