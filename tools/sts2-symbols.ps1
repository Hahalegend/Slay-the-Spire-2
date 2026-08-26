# sts2-symbols.ps1 -- collect what's needed to resolve the run-4 crash.
#
# Produces sts2_symbols.txt. Everything goes to that one file; upload it.
#
# Run from anywhere:   powershell -ExecutionPolicy Bypass -File .\sts2-symbols.ps1
# Or with explicit paths:
#   .\sts2-symbols.ps1 -Payload "C:\path\to\unstripped\libsts2.so"

param(
    [string]$Payload = "",
    [string]$Out     = "$PWD\sts2_symbols.txt"
)

$ErrorActionPreference = "Continue"

# --- the run-4 stack, already converted to file offsets ---------------------
# Atmosphere reports offsets from the executable segment, not from
# load_virtbase. Measured delta 0x3151000, constant across runs 3 and 4.
# Step 3 below re-derives it from the ELF so the numbers can be checked
# rather than trusted.
$Addrs = @(
  '0x3b73c94','0x3b7374c','0x3b7368c','0x3b73524','0x33c49a0',
  '0x3c4aa2c','0x3c4a8d4','0x33c50f8','0x33c792c','0x33c80e4',
  '0x33245e8','0x3c8a038','0x3c89fb8','0x5de26e8','0x325b714','0x32634ec'
)

# --- 1. locate the tools ----------------------------------------------------
function Find-Tool([string[]]$names) {
    foreach ($n in $names) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    $roots = @(
        "C:\devkitPro\devkitA64\bin", "C:\devkitpro\devkitA64\bin",
        "$env:DEVKITPRO\devkitA64\bin",
        "$env:ANDROID_NDK\toolchains\llvm\prebuilt\windows-x86_64\bin",
        "$env:ANDROID_NDK_HOME\toolchains\llvm\prebuilt\windows-x86_64\bin"
    )
    foreach ($r in $roots) {
        if (-not $r) { continue }
        foreach ($n in $names) {
            $p = Join-Path $r "$n.exe"
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

$addr2line = Find-Tool @('aarch64-none-elf-addr2line','llvm-addr2line','addr2line')
$nm        = Find-Tool @('aarch64-none-elf-nm','llvm-nm','nm')
$readelf   = Find-Tool @('aarch64-none-elf-readelf','llvm-readelf','readelf')

# --- 2. locate the payload --------------------------------------------------
# The UNSTRIPPED copy is the one that matters. Stripping removed the debug
# info but did not move anything (same LOAD span), so addresses from the
# running stripped binary resolve correctly against it.
$candidates = @()
if ($Payload) { $candidates += $Payload }
$candidates += @(
    ".\libsts2.so",
    "$env:USERPROFILE\Downloads\libsts2.so"
)
# anything ILC produced, newest first -- the publish dir copy is unstripped
$found = Get-ChildItem -Path C:\ -Filter "libsts2.so" -Recurse -ErrorAction SilentlyContinue -Force |
         Sort-Object Length -Descending | Select-Object -First 8
foreach ($f in $found) { $candidates += $f.FullName }

$lines = @()
$lines += "sts2_nx symbol dump -- $(Get-Date -Format s)"
$lines += "addr2line : $addr2line"
$lines += "nm        : $nm"
$lines += "readelf   : $readelf"
$lines += ""

if (-not $addr2line -and -not $nm) {
    $lines += "!! No aarch64 binutils found."
    $lines += "   Expected C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe"
    $lines += "   or the NDK's llvm-nm.exe. Set DEVKITPRO or ANDROID_NDK and rerun."
    $lines | Set-Content -Path $Out -Encoding utf8
    Write-Host "wrote $Out (no tools found)"
    exit 1
}

$lines += "candidate payloads:"
$seen = @{}
$usable = @()
foreach ($c in $candidates) {
    if (-not $c -or -not (Test-Path $c)) { continue }
    $full = (Resolve-Path $c).Path
    if ($seen.ContainsKey($full)) { continue }
    $seen[$full] = $true
    $sz = (Get-Item $full).Length
    $hasDebug = $false
    if ($readelf) {
        $sec = & $readelf -SW $full 2>$null | Out-String
        $hasDebug = $sec -match '\.debug_info'
        $hasSym   = $sec -match '\.symtab'
    }
    $lines += ("  {0,-70} {1,10:N0} bytes  debug={2} symtab={3}" -f $full, $sz, $hasDebug, $hasSym)
    if ($hasDebug -or $hasSym) { $usable += $full }
}
$lines += ""

if ($usable.Count -eq 0) {
    $lines += "!! Every copy found is fully stripped -- no .debug_info and no .symtab."
    $lines += "   The unstripped original is in the ILC publish output, something like:"
    $lines += "     aot\bin\Release\net9.0\linux-bionic-arm64\publish\*.so"
    $lines += "   Rerun with:  .\sts2-symbols.ps1 -Payload <that file>"
    $lines | Set-Content -Path $Out -Encoding utf8
    Write-Host "wrote $Out (no unstripped copy found)"
    exit 1
}

$target = $usable[0]
$lines += "using: $target"
$lines += ""

# --- 3. LOAD segments: re-derive the crash-offset delta ---------------------
# The first executable LOAD's vaddr is what Atmosphere reports offsets from.
# If this does not come out as 0x3151000, the addresses in step 4 are wrong
# and need recomputing -- which is the whole reason this section exists.
if ($readelf) {
    $lines += "=== LOAD segments (delta check) ==="
    $lines += (& $readelf -lW $target 2>$null | Select-String -Pattern 'LOAD' | Out-String).TrimEnd()
    $lines += ""
}

# --- 4. resolve the crash stack --------------------------------------------
if ($addr2line) {
    $lines += "=== addr2line: run-4 crash stack (first line = fault) ==="
    $res = & $addr2line -f -C -i -e $target $Addrs 2>&1
    $lines += ($res | Out-String).TrimEnd()
    $lines += ""
}

# --- 5. GC symbols, for re-deriving the nativeaot_shim hook offsets ---------
if ($nm) {
    $lines += "=== GC / startup symbols ==="
    $pat = 'GC_Initialize|CreateGCHandleManager|CreateGCHeap|PopulateDacVars|' +
           'PopulateHandleTableDacVars|g_theGCHeap|g_gc_heap_type|GCHeap|' +
           'RaiseFailFastException|LogUnhandledException|godotsharp_game_main_init|' +
           'StartupCodeHelpers|EnsureRuntimeInitialized'
    $res = & $nm -C --defined-only $target 2>&1 | Select-String -Pattern $pat
    $lines += ($res | Out-String).TrimEnd()
    $lines += ""

    $lines += "=== WKS::GCHeap vtable symbols ==="
    $res = & $nm -C --defined-only $target 2>&1 | Select-String -Pattern 'vtable for.*GCHeap|_ZTVN3WKS'
    $lines += ($res | Out-String).TrimEnd()
}

$lines | Set-Content -Path $Out -Encoding utf8
Write-Host "wrote $Out"
Write-Host "Upload that file."
