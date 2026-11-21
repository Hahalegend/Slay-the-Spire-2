<#
.SYNOPSIS
  Native Windows (no Docker/WSL) builder & SD-card assembler for sts2_nx.
  Uses portable .NET 9.0.317 SDK + Android NDK r27c to replicate scripts/docker-entry.sh
  and assembles the complete Switch homebrew folder ready to copy to sdmc:/switch/sts2_nx/.
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$ApkPath,

  [string]$SteamDir = "C:\Program Files (x86)\Steam\steamapps\common\Slay the Spire 2",
  [string]$OutDir   = "",
  [string]$ToolDir  = ""
)

$ErrorActionPreference = "Stop"
function Say($m)  { Write-Host "`n== $m ==" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "   [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "   [WARN] $m" -ForegroundColor Yellow }

$Repo      = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutDir))  { $OutDir  = Join-Path $Repo "dist\sts2_nx" }
if ([string]::IsNullOrWhiteSpace($ToolDir)) { $ToolDir = Join-Path $Repo "toolchains" }
$DotnetDir = Join-Path $ToolDir "dotnet-9.0.317"
$NdkDir    = Join-Path $ToolDir "android-ndk-r27c"
$NdkBin    = Join-Path $NdkDir "toolchains\llvm\prebuilt\windows-x86_64\bin"

if (-not (Test-Path (Join-Path $DotnetDir "dotnet.exe"))) { throw "Missing .NET 9.0.317 SDK at $DotnetDir" }
if (-not (Test-Path (Join-Path $NdkBin "clang.exe")))     { throw "Missing Android NDK r27c clang at $NdkBin" }
if (-not (Test-Path $ApkPath))                            { throw "APK or extracted folder not found: $ApkPath" }

$env:DOTNET_ROOT = $DotnetDir
$env:DOTNET_MULTILEVEL_LOOKUP = "0"
$env:ANDROID_NDK = $NdkDir
$env:PATH = "$DotnetDir;$NdkBin;$env:PATH"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 1. Extract APK if it's a file
$ApkWork = Join-Path $Repo "work\apk-extracted"
if (Test-Path $ApkPath -PathType Leaf) {
  Say "Extracting APK: $ApkPath"
  if (Test-Path $ApkWork) { Remove-Item $ApkWork -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $ApkWork | Out-Null
  tar.exe -xf $ApkPath -C $ApkWork
  $ApkDir = $ApkWork
} else {
  $ApkDir = (Resolve-Path $ApkPath).Path
}

# 2. Copy native .so libs & assets/ from APK into OutDir
Say "Assembling APK libraries & assets -> $OutDir"
$Arm64Dir = Join-Path $ApkDir "lib\arm64-v8a"
$RequiredArm64 = @(
  "libgodot_android.so",
  "libc++_shared.so",
  "libGodotFmod.android.template_release.arm64.so",
  "libspine_godot.android.template_release.arm64.so"
)
foreach ($lib in $RequiredArm64) {
  $src = Join-Path $Arm64Dir $lib
  if (-not (Test-Path $src)) { throw "Missing $lib in $Arm64Dir" }
  Copy-Item $src (Join-Path $OutDir $lib) -Force
  Ok $lib
}

# libfmod.so & libfmodstudio.so live in assets/native/ inside the APK (per source/config.h & fmod_shim.c)
foreach ($fmodLib in @("libfmod.so", "libfmodstudio.so")) {
  $cand1 = Join-Path $ApkDir "assets\native\$fmodLib"
  $cand2 = Join-Path $Arm64Dir $fmodLib
  if (Test-Path $cand1) {
    Copy-Item $cand1 (Join-Path $OutDir $fmodLib) -Force
    Ok "$fmodLib (from assets/native/)"
  } elseif (Test-Path $cand2) {
    Copy-Item $cand2 (Join-Path $OutDir $fmodLib) -Force
    Ok "$fmodLib (from lib/arm64-v8a/)"
  } else {
    Warn "$fmodLib not found in APK (audio may be silent)"
  }
}

$ApkAssets = Join-Path $ApkDir "assets"
if (Test-Path $ApkAssets) {
  $DstAssets = Join-Path $OutDir "assets"
  if (Test-Path $DstAssets) { Remove-Item $DstAssets -Recurse -Force }
  Copy-Item $ApkAssets $DstAssets -Recurse -Force
  Ok "assets/ directory copied"
} else {
  throw "Missing assets/ directory in APK at $ApkDir"
}

# 3. Collect Steam assemblies + extract GodotSharp.dll from APK libassembly-store.so
Say "Collecting Steam assemblies + APK GodotSharp.dll"
$WorkDir    = Join-Path $Repo "work\aot-build"
$RawDir     = Join-Path $WorkDir "raw"
$PatchedDir = Join-Path $WorkDir "patched"
$RefsDir    = Join-Path $WorkDir "refs"
if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $RawDir, $PatchedDir, $RefsDir | Out-Null

$SteamDllDir = Join-Path $SteamDir "data_sts2_windows_x86_64"
if (-not (Test-Path (Join-Path $SteamDllDir "sts2.dll"))) {
  $hit = Get-ChildItem $SteamDir -Recurse -Filter "sts2.dll" | Select-Object -First 1
  if (-not $hit) { throw "sts2.dll not found under $SteamDir" }
  $SteamDllDir = $hit.Directory.FullName
}
Copy-Item (Join-Path $SteamDllDir "*.dll") $RawDir -Force
Ok "Copied $((Get-ChildItem $RawDir -Filter *.dll).Count) Steam DLLs"

$StoreSo = Join-Path $Arm64Dir "libassembly-store.so"
if (-not (Test-Path $StoreSo)) { throw "Missing $StoreSo in APK" }
python (Join-Path $Repo "tools\extract_assembly_store.py") $StoreSo $RawDir "GodotSharp.dll"
if (-not (Test-Path (Join-Path $RawDir "GodotSharp.dll"))) { throw "Failed to extract GodotSharp.dll from $StoreSo" }
Ok "Replaced GodotSharp.dll from APK libassembly-store.so"

# 4. Stamp patchset, build STS2Switch.dll, and weave patches
Say "Weaving STS2Switch patches"
python (Join-Path $Repo "tools\stamp_patchset.py") $Repo
if ($LASTEXITCODE -ne 0) { throw "stamp_patchset.py failed with exit code $LASTEXITCODE" }

dotnet build (Join-Path $Repo "patches\STS2Switch\STS2Switch.csproj") -c Release -p:GameAssemblyDir="$RawDir" --nologo
if ($LASTEXITCODE -ne 0) { throw "STS2Switch.csproj build failed with exit code $LASTEXITCODE" }
$PatchDll = (Get-ChildItem (Join-Path $Repo "patches\STS2Switch\bin\Release") -Recurse -Filter "STS2Switch.dll" | Select-Object -First 1).FullName
if (-not $PatchDll) { throw "STS2Switch.dll was not produced" }

dotnet run --project (Join-Path $Repo "tools\StS2Patcher") -c Release -- --game "$RawDir" --patches "$PatchDll" --out "$PatchedDir" --strict
if ($LASTEXITCODE -ne 0) { throw "StS2Patcher failed with exit code $LASTEXITCODE" }

# 5. Filter framework & desktop-only assemblies (exact match to scripts/docker-entry.sh)
Say "Selecting AOT input assemblies"
$RefPack = (Get-ChildItem (Join-Path $DotnetDir "packs\Microsoft.NETCore.App.Ref\*\ref\net*") -Directory | Sort-Object FullName | Select-Object -Last 1).FullName
if (-not $RefPack) { throw "Could not locate Microsoft.NETCore.App.Ref in $DotnetDir" }
Write-Host "   framework list: $RefPack"

$DropExact = @(
  "coreclr.dll", "hostfxr.dll", "hostpolicy.dll", "mscordbi.dll", "mscorrc.dll", "msquic.dll",
  "steam_api64.dll", "System.IO.Compression.Native.dll", "WindowsBase.dll", "System.Windows.dll"
)
$kept = 0; $dropped = 0
foreach ($f in (Get-ChildItem $PatchedDir -Filter "*.dll")) {
  $n = $f.Name
  if (Test-Path (Join-Path $RefPack $n)) { $dropped++; continue }
  if ($n -like "System.Private.*" -or $n -like "clr*" -or $n -like "mscordaccore*" -or
      $n -like "Microsoft.DiaSymReader.Native.*" -or $n -like "Vortice.*" -or $n -like "SharpGen.*" -or
      $DropExact -contains $n) {
    $dropped++; continue
  }
  Copy-Item $f.FullName (Join-Path $RefsDir $n) -Force
  $kept++
}
Write-Host "   kept $kept, dropped $dropped"
Get-ChildItem $RefsDir -Filter "*.dll" | ForEach-Object { Write-Host "     $($_.Name)" }

foreach ($req in @("sts2.dll", "GodotSharp.dll", "STS2Switch.dll", "STS2Switch.Attributes.dll")) {
  if (-not (Test-Path (Join-Path $RefsDir $req))) { throw "Required assembly missing after filter: $req" }
}

# 6. Generate aot/rd.generated.xml (exact match to scripts/docker-entry.sh)
Say "Generating aot/rd.generated.xml"
$RdGen = Join-Path $Repo "aot\rd.generated.xml"
$rdLines = @(
  '<Directives xmlns="http://schemas.microsoft.com/netfx/2013/01/metadata">',
  '  <Application>',
  '    <!-- GENERATED by scripts/build-windows-native.ps1 -->'
)
foreach ($f in (Get-ChildItem $RefsDir -Filter "*.dll")) {
  $base = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
  if ($base -in @("sts2", "GodotSharp", "STS2Switch")) {
    $rdLines += "    <Assembly Name=`"$base`" Dynamic=`"Required All`" />"
  }
}
$rdLines += '  </Application>'
$rdLines += '</Directives>'
[System.IO.File]::WriteAllLines($RdGen, $rdLines, [System.Text.UTF8Encoding]::new($false))
Ok "Wrote $RdGen"

# 7. NativeAOT compile for linux-bionic-arm64 using all CPU cores
Say "Running NativeAOT compilation (dotnet 9.0.317 + Android NDK r27c -> linux-bionic-arm64)"
$AotProj = Join-Path $Repo "aot\sts2.aot.csproj"
$ObjDir = Join-Path $Repo "aot\obj"
$BinDir = Join-Path $Repo "aot\bin"
$StaleRaw = Join-Path $Repo "aot\assemblies-raw"
if (Test-Path $StaleRaw) { Remove-Item $StaleRaw -Recurse -Force }
if (Test-Path $ObjDir) { Remove-Item $ObjDir -Recurse -Force }
if (Test-Path $BinDir) { Remove-Item $BinDir -Recurse -Force }

dotnet publish $AotProj -c Release -r linux-bionic-arm64 --self-contained --nologo -p:AotReferenceDir="$RefsDir"
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed with exit code $LASTEXITCODE" }

$IlcRsp = Get-ChildItem $ObjDir -Recurse -Filter "*.ilc.rsp" | Select-Object -First 1
if ($IlcRsp -and (Select-String -Path $IlcRsp.FullName -Pattern "Vortice|SharpGen" -Quiet)) {
  throw "Vortice/SharpGen leaked into $($IlcRsp.FullName) -- desktop DirectX module initializers will crash on Switch!"
}

$SoFile = Get-ChildItem (Join-Path $Repo "aot\bin\Release") -Recurse -Filter "*.so" | Where-Object { $_.FullName -like "*publish*" } | Select-Object -First 1
if (-not $SoFile) { throw "ILC produced no .so under publish/" }
Ok "Produced: $($SoFile.FullName) ($([math]::Round($SoFile.Length/1MB, 1)) MB)"

$DistDir = Join-Path $Repo "dist"
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
Copy-Item $SoFile.FullName (Join-Path $DistDir "libsts2.so") -Force
Copy-Item $SoFile.FullName (Join-Path $OutDir "libsts2.so") -Force
$DbgFile = Get-ChildItem (Join-Path $Repo "aot\bin\Release") -Recurse -Filter "*.so.dbg" | Select-Object -First 1
if ($DbgFile) {
  Copy-Item $DbgFile.FullName (Join-Path $DistDir "libsts2.so.dbg") -Force
  Ok "Saved debug symbols to $DistDir\libsts2.so.dbg (not copied to SD card)"
}

# 8. Verify libsts2.so
Say "Verifying libsts2.so with tools/check_aot_so.py"
python (Join-Path $Repo "tools\check_aot_so.py") (Join-Path $OutDir "libsts2.so")
if ($LASTEXITCODE -ne 0) { throw "check_aot_so.py rejected libsts2.so" }

Say "Complete! Ready-to-copy Switch folder: $OutDir"
Get-ChildItem $OutDir | Format-Table Name, Mode, Length -AutoSize
