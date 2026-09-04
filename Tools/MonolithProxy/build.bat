@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\Scripts\build_proxy.ps1"
exit /b %ERRORLEVEL%
