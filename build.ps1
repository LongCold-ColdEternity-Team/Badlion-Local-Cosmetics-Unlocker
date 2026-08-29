param(
    [string]$JdkHome = 'C:\Program Files\Eclipse Adoptium\jdk-17.0.19.10-hotspot',
    [string]$AgentName = 'blc_unlock_agent.dll'
)
$ErrorActionPreference = 'Stop'
$vs = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat'
if (!(Test-Path $vs)) { throw "VsDevCmd.bat not found: $vs" }
if (!(Test-Path (Join-Path $JdkHome 'include\jni.h'))) { throw "JDK headers not found: $JdkHome" }
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'dist') | Out-Null
$root = $PSScriptRoot
$agentOutput = Join-Path $root ("bin\" + $AgentName)
$cmd = "call `"$vs`" -arch=x64 && pushd `"$root`" && cl /nologo /utf-8 /std:c++17 /EHsc /LD agent.cpp /I`"$JdkHome\include`" /I`"$JdkHome\include\win32`" /link /OUT:`"$agentOutput`" && cl /nologo /utf-8 /std:c++17 /EHsc injector.cpp /link /SUBSYSTEM:CONSOLE user32.lib psapi.lib /OUT:bin\BadlionUnlockInjector.exe && rc /nologo /fo bin\injector_ui.res injector_ui.rc && cl /nologo /utf-8 /std:c++17 /EHsc injector_ui.cpp bin\injector_ui.res /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib psapi.lib shell32.lib dwmapi.lib /OUT:bin\BadlionUnlockUI.exe && popd"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
Copy-Item (Join-Path $root 'target-classes.txt') (Join-Path $root 'bin\target-classes.txt') -Force
Copy-Item (Join-Path $root 'bin\BadlionUnlockUI.exe') (Join-Path $root 'dist\BadlionUnlockUI.exe') -Force
Write-Host "Built $root\bin\BadlionUnlockInjector.exe"
Write-Host "Built $root\bin\BadlionUnlockUI.exe"
Write-Host "Packaged single EXE $root\dist\BadlionUnlockUI.exe"
Write-Host "Built $agentOutput"
