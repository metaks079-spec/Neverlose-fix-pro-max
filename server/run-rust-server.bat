@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SERVER_DIR=%SCRIPT_DIR%rust-server"
set "EXE=%SERVER_DIR%\target\release\neverlose-server.exe"

if not exist "%SERVER_DIR%\Cargo.toml" (
    echo [ERROR] rust-server folder not found:
    echo         %SERVER_DIR%
    pause
    exit /b 1
)

cd /d "%SERVER_DIR%"

if exist "%EXE%" (
    echo [INFO] Starting neverlose-server.exe...
    "%EXE%"
    set "EXIT_CODE=%ERRORLEVEL%"
) else (
    where cargo >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] Binary not found and cargo is not installed/in PATH.
        echo         Expected binary: %EXE%
        pause
        exit /b 1
    )

    echo [INFO] Binary not found. Building and running with cargo...
    cargo run --release
    set "EXIT_CODE=%ERRORLEVEL%"
)

if not "%EXIT_CODE%"=="0" (
    echo.
    echo [ERROR] rust-server exited with code %EXIT_CODE%.
    pause
)

exit /b %EXIT_CODE%
