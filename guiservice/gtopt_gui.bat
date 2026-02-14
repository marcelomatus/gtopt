@echo off
REM gtopt_gui.bat - Windows launcher for gtopt GUI service
REM
REM This script is intended for use on Windows (not WSL).
REM For WSL, use the gtopt_gui shell script instead.

setlocal enabledelayedexpansion

REM Get the directory where this script is located
set "SCRIPT_DIR=%~dp0"

REM Find the guiservice directory
if exist "%SCRIPT_DIR%..\share\gtopt\guiservice\gtopt_gui.py" (
    set "GUISERVICE_DIR=%SCRIPT_DIR%..\share\gtopt\guiservice"
) else if exist "%SCRIPT_DIR%gtopt_gui.py" (
    set "GUISERVICE_DIR=%SCRIPT_DIR%"
) else (
    echo Error: Cannot find guiservice directory >&2
    exit /b 1
)

REM Find Python 3
set "PYTHON="
for %%p in (python3 python py) do (
    where %%p >nul 2>&1
    if !errorlevel! equ 0 (
        REM Check if it's Python 3.10+
        %%p -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>&1
        if !errorlevel! equ 0 (
            set "PYTHON=%%p"
            goto :python_found
        )
    )
)

echo Error: Python 3.10 or later is required >&2
echo Please install Python 3.10+ from https://www.python.org/ >&2
exit /b 1

:python_found

REM Check if required Python packages are installed
%PYTHON% -c "import flask, pandas, pyarrow, requests" >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: Required Python packages are not installed >&2
    echo.
    echo Please install the required packages: >&2
    echo   %PYTHON% -m pip install -r "%GUISERVICE_DIR%\requirements.txt" >&2
    echo.
    echo Or create a virtual environment: >&2
    echo   %PYTHON% -m venv %USERPROFILE%\.gtopt-gui-venv >&2
    echo   %USERPROFILE%\.gtopt-gui-venv\Scripts\activate >&2
    echo   pip install -r "%GUISERVICE_DIR%\requirements.txt" >&2
    exit /b 1
)

REM Run the Python launcher
%PYTHON% "%GUISERVICE_DIR%\gtopt_gui.py" %*
