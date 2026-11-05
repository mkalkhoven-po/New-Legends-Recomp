@echo off
rem Configure and build with clang-cl + Ninja inside the VS 2022 x64 environment.
rem Usage: build.bat [Release|RelWithDebInfo|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul || exit /b 1
set CLANG="C:/Program Files/LLVM/bin/clang-cl.exe"

if not exist build\build.ninja (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
        -DCMAKE_C_COMPILER=%CLANG% -DCMAKE_CXX_COMPILER=%CLANG% || exit /b 1
)
cmake --build build || exit /b 1
