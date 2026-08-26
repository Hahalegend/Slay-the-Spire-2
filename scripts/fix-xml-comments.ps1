# fix-xml-comments.ps1 -- make every XML comment in the project files legal.
#
# XML forbids "--" inside a comment body and a trailing "-" before "-->".
# Prose written with em-dash-style "--" separators, and "===" banner rules
# ending in dashes, both violate it. MSBuild reports this as:
#
#   error MSB4025: The project file could not be loaded. An XML comment cannot
#   contain '--', and '-' cannot be the last character.
#
# and it stops at the FIRST offending file, so fixing them one at a time costs
# a rebuild each. This does all of them at once.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$files = Get-ChildItem -Path $root -Recurse -Include *.csproj,*.props,*.xml |
         Where-Object { $_.FullName -notmatch '\\(bin|obj|apk|dist)\\' }

$fixed = 0
foreach ($f in $files) {
    $text = Get-Content -Raw -Path $f.FullName
    if (-not $text) { continue }

    $out = [regex]::Replace($text, '(?s)<!--(.*?)-->', {
        param($m)
        $body = $m.Groups[1].Value
        # banner rules: '=====----' -> all '='
        $body = [regex]::Replace($body, '={3,}-*', { param($x) '=' * $x.Value.Length })
        # any run of 2+ dashes -> en dash
        $body = [regex]::Replace($body, '-{2,}', [char]0x2013)
        # no trailing '-' before the close
        $body = [regex]::Replace($body, '-(\s*)$', '$1')
        "<!--$body-->"
    })

    if ($out -ne $text) {
        Set-Content -Path $f.FullName -Value $out -Encoding UTF8 -NoNewline
        Write-Host "  fixed $($f.FullName.Substring($root.Length+1))"
        $fixed++
    }
}

Write-Host "`n$fixed file(s) fixed."

# Verify: every one must now load as XML.
$bad = 0
foreach ($f in $files) {
    try   { [xml](Get-Content -Raw -Path $f.FullName) | Out-Null }
    catch { Write-Host "  STILL BROKEN: $($f.Name) -- $($_.Exception.Message)" -ForegroundColor Red; $bad++ }
}
if ($bad -eq 0) { Write-Host "All project files parse as valid XML." -ForegroundColor Green }
