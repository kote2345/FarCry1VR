@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
"C:\Users\user\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" --target=x86_64-pc-windows-msvc -fuse-ld="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/link.exe" -std=c++17 -O2 -w -ffunction-sections -DWIN32 -DWIN64 -DDONT_USE_CRY_MEMORY_MANAGER -D_CRT_SECURE_NO_WARNINGS -ISourceCode/CryPhysics -ISourceCode/CryCommon research/native_grip_solver.generated.cpp -c -o research/native_grip_solver.obj
if errorlevel 1 exit /b 1
link /NOLOGO /OPT:REF /OUT:research/native_grip_solver.exe research/native_grip_solver.obj libcmt.lib oldnames.lib kernel32.lib user32.lib
