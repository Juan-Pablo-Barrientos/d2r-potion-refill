# Builds d2rl-potion-refill.dll with MSVC (Visual Studio 2022 C++ tools).
#   .\build.ps1            build into .\build
#   .\build.ps1 -Install   build and copy the DLL into the game's plugin folder
param(
	[switch]$Install,
	[string]$GameDir = "D:\Diablo II Resurrected"
)
$ErrorActionPreference = "Stop"

function Find-Toolchain {
	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { $null }
	if (-not $vs) { throw "Visual Studio with the C++ tools was not found." }

	$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
	$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
	$kitsVer = Get-ChildItem "$kits\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\um\windows.h" } |
		Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
	if (-not $msvc -or -not $kitsVer) { throw "MSVC or the Windows 10/11 SDK was not found." }

	# A complete install has the C++ headers next to the compiler. Some trimmed
	# installs only have the older copy bundled with ScopeCppSDK.
	$stl = "$msvc\include"
	if (-not (Test-Path "$stl\vcruntime.h")) { $stl = "$vs\SDK\ScopeCppSDK\vc15\VC\include" }
	$crtLib = "$msvc\lib\x64"
	if (-not (Test-Path "$crtLib\libcmt.lib")) { $crtLib = "$msvc\lib\onecore\x64" }

	$env:PATH    = "$msvc\bin\Hostx64\x64;$kits\bin\$kitsVer\x64;$env:PATH"
	$env:INCLUDE = "$stl;$kits\Include\$kitsVer\ucrt;$kits\Include\$kitsVer\um;$kits\Include\$kitsVer\shared"
	$env:LIB     = "$crtLib;$kits\Lib\$kitsVer\ucrt\x64;$kits\Lib\$kitsVer\um\x64"
}

Find-Toolchain

$root  = $PSScriptRoot
$out   = Join-Path $root "build"
$sdk   = Join-Path $root "third_party\PluginSDK\include"
$name  = "d2rl-potion-refill"
New-Item -ItemType Directory -Force $out | Out-Null

& rc.exe /nologo /i $sdk /fo "$out\$name.res" "$root\src\potion_refill_plugin.rc"
if ($LASTEXITCODE -ne 0) { throw "rc failed" }

& cl.exe /nologo /std:c++20 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
	/I $sdk /Fo"$out\\" /c "$root\src\potion_refill_plugin.cpp"
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

& link.exe /nologo /DLL /OUT:"$out\$name.dll" "$out\potion_refill_plugin.obj" "$out\$name.res" kernel32.lib
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Host "Built $out\$name.dll"

if ($Install) {
	$plugins = Join-Path $GameDir "d2rloader\plugins"
	Copy-Item -Force "$out\$name.dll" $plugins
	Write-Host "Installed to $plugins"
}
