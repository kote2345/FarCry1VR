@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0"
if errorlevel 1 exit /b 1

rem Use the existing Android configuration and build ALL native targets.
rem CMake/Ninja rebuilds changed sources and every dependent shared library.
if not exist "build_android\arm64-v8a\CMakeCache.txt" (
    echo ERROR: Configure the Android ARM64 CMake build first.
    goto failed
)

set "FCVR_CMAKE="
for /f "usebackq tokens=1,* delims==" %%A in ("build_android\arm64-v8a\CMakeCache.txt") do (
    if "%%A"=="CMAKE_COMMAND:INTERNAL" set "FCVR_CMAKE=%%B"
)
if not defined FCVR_CMAKE (
    echo ERROR: CMAKE_COMMAND is missing from the native build cache.
    goto failed
)
if not exist "%FCVR_CMAKE%" (
    echo ERROR: CMake from the native build cache was not found: "%FCVR_CMAKE%"
    goto failed
)

if not defined ANDROID_HOME if defined ANDROID_SDK_ROOT set "ANDROID_HOME=%ANDROID_SDK_ROOT%"
if not defined ANDROID_HOME set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
if not defined ANDROID_SDK_ROOT set "ANDROID_SDK_ROOT=%ANDROID_HOME%"

echo [1/2] Building all native libraries...
"%FCVR_CMAKE%" --build "build_android\arm64-v8a" --parallel
if errorlevel 1 goto failed

rem CryVR and its embedded shaders are present in BOTH of these modules.
rem Never package only an updated XRenderVulkan with an old CrySystem.
if not exist "bin\arm64-Release\libCrySystem.so" (
    echo ERROR: Native build did not produce libCrySystem.so.
    goto failed
)
if not exist "bin\arm64-Release\libXRenderVulkan.so" (
    echo ERROR: Native build did not produce libXRenderVulkan.so.
    goto failed
)

echo [2/2] Synchronizing all native libraries and building the release APK...
rem Gradle's syncEngineNativeLibraries copies the complete native output
rem before merging JNI libraries and stripping the packaged binaries.
call "android\gradlew.bat" -p "android" :app:assembleRelease
if errorlevel 1 goto failed

echo.
echo APK: "%CD%\android\app\build\outputs\apk\release\app-release.apk"
popd
exit /b 0

:failed
echo.
echo BUILD FAILED. Do not install an APK from a previous build.
popd
exit /b 1
