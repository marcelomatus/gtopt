@echo off
REM gtopt_websrv.bat - Windows launcher for gtopt web service
REM
REM This script is intended for use on Windows.
REM For WSL, use the gtopt_websrv shell script instead.

setlocal enabledelayedexpansion

REM Get the directory where this script is located
set "SCRIPT_DIR=%~dp0"

REM Find the webservice directory
if exist "%SCRIPT_DIR%..\share\gtopt\webservice\gtopt_websrv.js" (
    set "WEBSERVICE_DIR=%SCRIPT_DIR%..\share\gtopt\webservice"
) else if exist "%SCRIPT_DIR%gtopt_websrv.js" (
    set "WEBSERVICE_DIR=%SCRIPT_DIR%"
) else (
    echo Error: Cannot find webservice directory >&2
    exit /b 1
)

REM Find Node.js
set "NODE="
for %%p in (node.exe node) do (
    where %%p >nul 2>&1
    if !errorlevel! equ 0 (
        REM Check if it's Node.js 18+
        for /f "tokens=1 delims=." %%v in ('%%p --version 2^>nul') do (
            set "VERSION=%%v"
            set "VERSION=!VERSION:v=!"
            if !VERSION! geq 18 (
                set "NODE=%%p"
                goto :node_found
            )
        )
    )
)

echo Error: Node.js 18 or later is required >&2
echo Please install Node.js from https://nodejs.org/ >&2
exit /b 1

:node_found

REM Run the Node.js launcher
%NODE% "%WEBSERVICE_DIR%\gtopt_websrv.js" %*
