@echo off
if not defined VCINSTALLDIR (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars32.bat" >nul
)
where cl >nul 2>nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\atmosphere-test-obj mkdir build\atmosphere-test-obj
cl /nologo /EHsc /std:c++20 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\AtmosphereRegression.cpp src\Effects\DirectionalVolumetricLighting.cpp src\Effects\LocalLightingRenderer.cpp src\Lighting\LocalLightManager.cpp src\Lighting\ActorLightManager.cpp src\Game\WoWClientContext.cpp src\D3D9\DepthCapture.cpp src\Core\FrameContext.cpp src\Core\ShaderCache.cpp src\Diagnostics\RendererDiagnostics.cpp src\Diagnostics\PerformanceProfiler.cpp /Febuild\Release\AtmosphereRegression.exe /Fobuild\atmosphere-test-obj\ /link user32.lib ole32.lib d3d9.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
build\Release\AtmosphereRegression.exe
