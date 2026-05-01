@echo off
chcp 65001 >nul
setlocal

echo ================================================
echo    NeverLose Local Server Launcher
echo ================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "SERVER_DIR=%SCRIPT_DIR%rust-server"
set "EXE=%SERVER_DIR%\target\release\neverlose-server.exe"

:: Проверка существования сервера
if not exist "%SERVER_DIR%\Cargo.toml" (
    echo [ERROR] Папка rust-server не найдена:
    echo         %SERVER_DIR%
    echo.
    pause
    exit /b 1
)

cd /d "%SERVER_DIR%"

:: Загрузка .env если существует
if exist ".env" (
    echo [INFO] Найден .env файл
) else (
    echo [WARN] .env файл не найден, используются значения по умолчанию
)

echo.
echo [INFO] Конфигурация сервера:
echo        - HTTP Port: 30031 (для Requestor/Lua libraries)
echo        - WebSocket Port: 30030 (для live updates)
echo        - Bind Address: 0.0.0.0 (доступен со всех интерфейсов)
echo        - Auth Bypass: TRUE (не требуется токен)
echo.

:: Проверка наличия скомпилированного exe
if exist "%EXE%" (
    echo [INFO] Запуск neverlose-server.exe...
    echo ================================================
    echo.
    "%EXE%"
    set "EXIT_CODE=%ERRORLEVEL%"
) else (
    :: Если exe нет, пытаемся собрать через cargo
    where cargo >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] Бинарник не найден и cargo не установлен.
        echo         Ожидаемый путь: %EXE%
        echo.
        echo         Установи Rust: https://rustup.rs/
        echo.
        pause
        exit /b 1
    )

    echo [INFO] Бинарник не найден. Сборка через cargo...
    echo         Это может занять несколько минут при первой сборке...
    echo ================================================
    echo.
    cargo run --release
    set "EXIT_CODE=%ERRORLEVEL%"
)

echo.
echo ================================================
if not "%EXIT_CODE%"=="0" (
    echo [ERROR] Сервер завершился с кодом ошибки %EXIT_CODE%
    echo.
    pause
) else (
    echo [INFO] Сервер остановлен
)

exit /b %EXIT_CODE%
