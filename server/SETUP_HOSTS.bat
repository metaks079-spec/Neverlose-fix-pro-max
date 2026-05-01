@echo off
chcp 65001 >nul

echo ================================================
echo    Настройка hosts файла для локального сервера
echo ================================================
echo.
echo Этот скрипт добавит в hosts файл перенаправление:
echo    162.19.230.28 → 127.0.0.1
echo.
echo Нужны права администратора!
echo.
pause

:: Проверка прав администратора
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] Требуются права администратора!
    echo        Запусти этот файл ПРАВОЙ кнопкой → "Запуск от имени администратора"
    echo.
    pause
    exit /b 1
)

set "HOSTS_FILE=%SystemRoot%\System32\drivers\etc\hosts"
set "ENTRY=127.0.0.1    162.19.230.28"

echo [INFO] Проверка hosts файла: %HOSTS_FILE%
echo.

:: Проверка, есть ли уже такая запись
findstr /C:"%ENTRY%" "%HOSTS_FILE%" >nul 2>&1
if %errorLevel% equ 0 (
    echo [INFO] Запись уже существует в hosts файле!
    echo        %ENTRY%
    echo.
    echo [OK] Ничего делать не нужно.
    echo.
    pause
    exit /b 0
)

:: Проверка, есть ли запись с этим IP но другим комментарием
findstr /C:"162.19.230.28" "%HOSTS_FILE%" >nul 2>&1
if %errorLevel% equ 0 (
    echo [WARN] Найдена существующая запись для 162.19.230.28
    echo        Проверь hosts файл вручную!
    echo.
    notepad "%HOSTS_FILE%"
    pause
    exit /b 1
)

:: Добавление записи
echo [INFO] Добавление записи в hosts файл...
echo.

:: Бэкап hosts файла
copy "%HOSTS_FILE%" "%HOSTS_FILE%.backup_%date:~-4,4%%date:~-7,2%%date:~-10,2%" >nul 2>&1
if %errorLevel% equ 0 (
    echo [INFO] Создан бэкап: %HOSTS_FILE%.backup_%date:~-4,4%%date:~-7,2%%date:~-10,2%
)

:: Добавление записи
echo. >> "%HOSTS_FILE%"
echo # NeverLose local server redirect >> "%HOSTS_FILE%"
echo %ENTRY% >> "%HOSTS_FILE%"

if %errorLevel% equ 0 (
    echo [OK] Запись успешно добавлена!
    echo.
    echo Теперь все подключения к 162.19.230.28 будут перенаправляться на 127.0.0.1
    echo.
    echo Содержимое hosts файла:
    echo ================================================
    type "%HOSTS_FILE%"
    echo ================================================
) else (
    echo [ERROR] Не удалось добавить запись в hosts файл!
    echo.
    notepad "%HOSTS_FILE%"
)

echo.
echo [INFO] Готово! Теперь запусти START_SERVER.bat
echo.
pause
