param([string]$Name = 'spray_runtime_probe')
$ErrorActionPreference = 'Stop'
$jdk = 'C:/Program Files/Eclipse Adoptium/jdk-17.0.19.10-hotspot'
$vs = 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/Tools/VsDevCmd.bat'
$out = Join-Path $PSScriptRoot 'probe-bin'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$src = Join-Path $PSScriptRoot 'runtime_probe.cpp'
$cmd = "call `"$vs`" -arch=x64 && cl /nologo /utf-8 /std:c++17 /EHsc /LD `"$src`" /I`"$jdk/include`" /I`"$jdk/include/win32`" /Fo`"$out/$Name.obj`" /link /OUT:`"$out/$Name.dll`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Probe build failed: $LASTEXITCODE" }
