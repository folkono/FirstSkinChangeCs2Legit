@echo off
rem Builds native\build\Release\vmoverlay.exe (needs Visual Studio 2022 Build Tools + CMake)
cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build "%~dp0build" --config Release -- /m /v:minimal || exit /b 1
echo.
echo Pronto: %~dp0build\Release\vmoverlay.exe
