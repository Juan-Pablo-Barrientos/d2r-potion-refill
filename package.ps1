# Builds the release files into .\dist:
#   d2rl-potion-refill.dll      the plugin
#   PotionRefill-D2RMM.zip      the D2RMM mod folder with the plugin and install notes
param(
	# Package this DLL instead of building one, e.g. the copy tested in game.
	[string]$Dll
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$dist = Join-Path $root "dist"
if (-not $Dll) {
	& (Join-Path $root "build.ps1")
	$Dll = Join-Path $root "build\d2rl-potion-refill.dll"
}

Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
$stage = Join-Path $dist "stage\PotionRefill"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$root\d2rmm\PotionRefill\*" $stage
Copy-Item $Dll $stage
Copy-Item $Dll $dist

Compress-Archive -Path $stage -DestinationPath "$dist\PotionRefill-D2RMM.zip"
Remove-Item -Recurse -Force (Join-Path $dist "stage")
Get-ChildItem $dist | ForEach-Object { Write-Host "$($_.Name)  $($_.Length) bytes" }
