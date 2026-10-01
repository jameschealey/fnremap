@echo off
setlocal

set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "WDKLIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"
set "WDKINC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%WDKINC%\ucrt;%WDKINC%\um;%WDKINC%\shared"
set "LIB=%MSVC%\lib\x64;%WDKLIB%\ucrt\x64;%WDKLIB%\um\x64"

cd /d "%~dp0"
cl /nologo /W4 /Ox fnremap.c /link winusb.lib setupapi.lib user32.lib wtsapi32.lib userenv.lib advapi32.lib
