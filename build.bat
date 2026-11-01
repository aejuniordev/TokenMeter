@echo off
rem Compila o TokenMeter.exe. Use UM dos compiladores abaixo:
rem   - Visual Studio Build Tools: abra o "Developer Command Prompt" e rode build.bat
rem   - MinGW-w64 (MSYS2, w64devkit, winget install BrechtSanders.WinLibs.POSIX.UCRT): rode build.bat
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto msvc
where gcc >nul 2>nul
if %errorlevel%==0 goto mingw

echo Nenhum compilador encontrado (cl ou gcc).
echo Instale o Visual Studio Build Tools ou o MinGW-w64 e rode de novo.
exit /b 1

:msvc
echo Compilando com MSVC...
rc /nologo tokenmeter.rc || exit /b 1
cl /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS tokenmeter.c tokenmeter.res /Fe:TokenMeter.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup || exit /b 1
del /q tokenmeter.obj 2>nul
goto ok

:mingw
echo Compilando com MinGW...
windres tokenmeter.rc -O coff -o tokenmeter.res || exit /b 1
gcc -O2 -s -municode -mwindows tokenmeter.c tokenmeter.res -o TokenMeter.exe -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lcomctl32 -lole32 -luuid || exit /b 1
goto ok

:ok
echo.
echo Pronto: TokenMeter.exe
