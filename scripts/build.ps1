# build.ps1 — generate + build the native projects (and optionally the server).
#
#   pwsh scripts/build.ps1            # build everything (Release x64) + server
#   pwsh scripts/build.ps1 -Core      # just core.dll
#   pwsh scripts/build.ps1 -NoServer  # native only
param(
    [switch]$Core,
    [switch]$NoServer,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$premake = (Get-Command premake5 -ErrorAction SilentlyContinue)?.Source
if (-not $premake) { $premake = "$env:USERPROFILE\.local\bin\premake5.exe" }
if (-not (Test-Path $premake)) { throw "premake5 not found on PATH" }

& $premake vs2022 | Out-Null

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found — install VS2022 C++ tools" }

$target = if ($Core) { "core.vcxproj" } else { "gmod-mcp.sln" }
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root\build`" && msbuild $target /p:Configuration=$Config /p:Platform=x64 /m /v:minimal /nologo"
if ($LASTEXITCODE -ne 0) { throw "native build failed" }

if (-not $NoServer -and -not $Core) {
    Push-Location "$root\server"
    npm install | Out-Null
    npm run build
    Pop-Location
}

Write-Host "`nBuilt -> $root\build\bin\$Config" -ForegroundColor Green
