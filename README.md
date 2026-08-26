# Slay the Spire 2 — Nintendo Switch port (Godot + NativeAOT wrapper)

This is a native wrapper / loader that runs the original ARM64 Android build of
Slay the Spire 2 on Switch homebrew. It contains no game code and no game
assets. It loads the game's own engine library (`libgodot_android.so`) and
recreates, natively, the Android/JNI layer Godot expects — a fake
JNIEnv/JavaVM, a Vulkan context on the console's own window, an audio device,
input, and the Java-side classes the engine calls into.

The game is C#, so the wrapper also stands in for GodotSharp's Mono/CoreCLR
path: the managed assemblies are compiled ahead of time into `libsts2.so` and
loaded in place of the JIT the engine would otherwise expect.

## Install & run

You need the `.pck` from a Steam install, plus four libraries from the Android
APK. Copy the `.nro` to your SD card and place the game files next to it:

https://github.com/Ekyso/StS2-Launcher
You need latest .apk (v0.3.14) from Cinderbox Discord

```
sdmc:/switch/sls2_nx
├── sts2.nro
├── sts2.pck                                            <- from your Steam install
├── libgodot_android.so                                 <- from your APK: lib/arm64-v8a/
├── libc++_shared.so                                    <- from your APK: lib/arm64-v8a/
├── libGodotFmod.android.template_release.arm64.so      <- from your APK: lib/arm64-v8a/
├── libspine_godot.android.template_release.arm64.so    <- from your APK: lib/arm64-v8a/
├── libsts2.so                                          <- built; see Building
├── assets/                                             <- from your APK
└── save/                                               <- created on first boot
```

## First boot

The first boot converts the game's textures to ASTC and writes
`save/sts2_astc.pck`. It takes several minutes with the CPU boosted to
1785 MHz, shows progress on screen, and happens once — later boots find the
file and skip it. This will take 20-30 minutes.


## Credits

Forked from `drmariomania_nx`, inheriting the `so_util`/JNI-shim lineage
(Andy Nguyen, fgsfds, NaGaa95, elliencode) — all MIT-licensed.
Thanks @delsonazevedo for AOT work,
Ekyso for initial Android port

The first-boot progress console and the heap poison/quarantine allocator came
from ChanseyIsTheBest's Switch port of Fruit Ninja. Its GC bridge is
Boehm/IL2CPP and does not apply here; NativeAOT's cooperative GC needed
different handling.

Vulkan runs on Mesa's NVK with a Switch backend (`nvkmd-switch`, `wsi_switch`).

Slay the Spire 2 is © Mega Crit. No game code or assets are distributed here.
