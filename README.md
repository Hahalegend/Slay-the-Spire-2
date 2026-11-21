# Slay the Spire 2 — Nintendo Switch port (Godot + NativeAOT wrapper)

> **Fork highlights (Steam `v0.111.1` + Switch Performance & Loading Optimizations):**
> - Updated patches for Steam **`v0.111.1`** (`SentryGodotInitializer`, `ModelDbPatches` `AbstractModel..ctor` + Fabricator minions, `LifecyclePatches` fallback).
> - Major loading & FPS improvements (`PreloadPatches.cs` selective preloading + parallel core combat VFX, GPU 76.8 MHz `cpu_boost` fix in `source/util.c` + `scripts/patch-nro-gpu-clock.py`, tuned `configs/config.txt` & `configs/override.cfg`).
> - Native Windows NativeAOT build script without Docker/WSL (`scripts/build-windows-native.ps1`).
> - See **[docs/OPTIMIZATIONS_V0111.md](docs/OPTIMIZATIONS_V0111.md)** for full details and build instructions.

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
# Building AOT payload

# Building `libsts2.so Only` V0.107.1 tested
 
**You have to do this yourself. It cannot be downloaded.**
 
`libsts2.so` is the game's own C# code, compiled for the Switch. It is Mega
Crit's code, so nobody can distribute it for you — but you already own it, and
this turns your copy into the file the port needs.
 
 ---
 
## What you need first
 
**1. Slay the Spire 2 installed on Steam.**
 
**2. The Android APK, unzipped.** This is the phone version of the game. You
need one file out of it. If you have the `.apk`, rename it to `.zip` and
extract it like any other zip — Windows will do this from the right-click menu.
 
**3. Docker Desktop.** Free, from <https://www.docker.com/products/docker-desktop/>.
 
Install it and **restart your PC when it asks**. Open Docker Desktop once and
wait until it says *Engine running* in the bottom-left corner. Leave it open
while you work. If it asks to install WSL2, say yes and let it finish.
 
**4. This project's folder**, unzipped somewhere without spaces in the path.
`C:\devkitPro\sts2_nx` is a good choice. `C:\Users\Your Name\Desktop\sts2 nx`
is not — the space will break the build in a confusing way.
 
---
 
## Step 1 — Find your two folders
 
**Your Steam install.** In Steam, right-click Slay the Spire 2 → *Manage* →
*Browse local files*. A folder opens. Copy the path from the address bar.
It usually looks like:
 
```
C:\Program Files (x86)\Steam\steamapps\common\Slay the Spire 2
```
 
**Your unzipped APK.** Wherever you extracted it. You want the folder that
*contains* the `assets` and `lib` folders inside it.
 
Keep both paths handy. Spaces in these are fine.
 
---
 
## Step 2 — Collect the game's code files
 
Open **PowerShell** (press Start, type `powershell`, press Enter) and run these
two lines, replacing the paths with your own:
 
```powershell
cd C:\devkitPro\sts2_nx
 
.\scripts\collect-assemblies.ps1 -SteamDir "C:\Program Files (x86)\Steam\steamapps\common\Slay the Spire 2" -ApkDir "C:\Users\You\Downloads\sts2-apk"
```
 
Keep the quotes around the paths — they are what make spaces work.
 
This gathers about 200 files into `aot\assemblies-raw`. It takes a few seconds.
 
> **Why two sources?** The game's code comes from Steam, but one file —
> `GodotSharp.dll` — has to come from the APK instead. Steam's copy is built for
> PC and does not match the engine the Switch port uses. The script handles this
> for you; it is only worth knowing because it is why you need both.
 
If it says `not found`, the path is wrong — check for a typo, and check the
folder actually exists in Explorer.
 
---
 
## Step 3 — Build the Docker image
 
Still in PowerShell, in the same folder:
 
```powershell
docker build -f scripts/Dockerfile.aot -t sts2nx-aot .
```
 
The first time, this downloads about 2 GB and takes 5–15 minutes depending on
your connection. Later runs take seconds.
 
**Run this every time**, even when nothing seems to have changed. It copies the
project files into the image, so skipping it silently builds the old version and
you get a file that looks fine and is not.
 
---
 
## Step 4 — Build `libsts2.so`
 
```powershell
docker run --rm -it -v "C:\devkitPro\sts2_nx\aot\assemblies-raw:/in:ro" -v "C:\devkitPro\sts2_nx\dist:/out" sts2nx-aot
```
 
That is one long line — copy it whole. If your project is somewhere other than
`C:\devkitPro\sts2_nx`, change **both** paths.
 
This takes 15–30 minutes. Text will scroll past continuously; that is normal.
 
**You want to see this at the end:**
 
```
applied 36, skipped 0, failed 0
```
 
`failed 0` is the part that matters. If it says anything else, stop and see
*When it goes wrong* below.
 
---
 
## Step 5 — Copy the file across
 
The finished file is at:
 
```
C:\devkitPro\sts2_nx\dist\libsts2.so
```
 
Copy it to your SD card, next to `sts2.nro`:
 
```
sdmc:/switch/sls2_nx/libsts2.so
```


## First boot

The first boot converts the game's textures to ASTC and writes
`save/sts2_astc.pck`. It takes several minutes with the CPU boosted to
1785 MHz, shows progress on screen, and happens once — later boots find the
file and skip it. This will take 20-30 minutes.

## Building

**The NRO** needs devkitPro with the `switch-dev` group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib \
          switch-libwebp switch-zstd switch-libelf switch-libexpat
make
```

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
