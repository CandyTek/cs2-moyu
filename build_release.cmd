@echo off
chcp 65001 >nul
:: Use vswhere to find the latest VS installation path
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set VS_PATH=%%i
)

:: call VsDevCmd.bat
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -no_logo

cmake -S . -B cmake-build-release-ninja-vs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl && cmake --build cmake-build-release-ninja-vs --config Release --target CSMoyu -j 18
