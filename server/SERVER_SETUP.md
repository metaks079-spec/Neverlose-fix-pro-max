# NeverLose Local Server Setup

## 🚀 Быстрый старт

### Вариант 1: Использовать готовый exe (РЕКОМЕНДУЕТСЯ)

Сервер уже скомпилирован! Просто запусти:

```batch
START_SERVER.bat
```

Сервер запустится на:
- **HTTP**: `http://0.0.0.0:30031` (для Lua libraries, configs)
- **WebSocket**: `ws://0.0.0.0:30030` (для live updates)

### Вариант 2: Собрать из исходников

Если хочешь пересобрать сервер:

1. Установи Rust: https://rustup.rs/
2. Запусти:
```batch
cd rust-server
cargo build --release
```

## 📋 Требования

### Обязательные:
- ✅ Windows 10/11
- ✅ Скомпилированный exe (уже есть в `rust-server/target/release/`)

### Опциональные:
- PostgreSQL (если хочешь использовать БД)
- Rust toolchain (если хочешь пересобрать)

## ⚙️ Конфигурация

Файл `.env` в папке `rust-server/`:

```env
# Server ports
HTTP_PORT=30031      # Для HTTP запросов (Requestor)
WS_PORT=30030        # Для WebSocket подключений

# Bind address
BIND_ADDR=0.0.0.0    # 0.0.0.0 = доступен отовсюду, 127.0.0.1 = только локально

# Auth bypass для крака
AUTH_BYPASS=true     # Не требует токен авторизации

# Default serial (хардкод из Requestor.cpp)
DEFAULT_SERIAL=g6w/cgN2AuDsLw3xrzboM1kbkLy+osvg0Y/j0LJnQf04GHbV...
```

## 🔧 Настройка клиента (DLL)

Клиент уже настроен на подключение к `162.19.230.28`. Чтобы он подключался к твоему локальному серверу, нужно:

### Вариант 1: Изменить hosts файл (БЕЗ ПЕРЕСБОРКИ)

1. Открой блокнот **от имени администратора**
2. Открой файл: `C:\Windows\System32\drivers\etc\hosts`
3. Добавь строку:
   ```
   127.0.0.1    162.19.230.28
   ```
4. Сохрани

Теперь когда DLL будет подключаться к `162.19.230.28`, она попадёт на твой локальный сервер!

### Вариант 2: Изменить IP в коде (ТРЕБУЕТ ПЕРЕСБОРКИ)

Измени в `Requestor.cpp`:
```cpp
// Было:
hConnection = WinHttpConnect(hSession, L"162.19.230.28", 30031, 0);

// Стало:
hConnection = WinHttpConnect(hSession, L"127.0.0.1", 30031, 0);
```

И в `setup_hooks.cpp` и `prod_setup.cpp`:
```cpp
// Было:
*ppNodeName = "162.19.230.28";

// Стало:
*ppNodeName = "127.0.0.1";
```

## 🗂️ Структура API

Сервер предоставляет следующие endpoints:

### HTTP API (Port 30031)

#### GET `/lua/{library_name}?cheat=csgo`
Возвращает Lua библиотеку

**Пример:**
```
GET http://127.0.0.1:30031/lua/ffi?cheat=csgo
```

#### POST `/api/auth`
Авторизация (байпасится если `AUTH_BYPASS=true`)

#### GET `/api/configs`
Получить список конфигов пользователя

#### POST `/api/configs`
Сохранить конфиг

#### GET `/api/scripts`
Получить список скриптов

### WebSocket API (Port 30030)

Для real-time обновлений (configs, scripts, etc.)

**Подключение:**
```
ws://127.0.0.1:30030/
```

## 📦 База данных (опционально)

Если хочешь использовать PostgreSQL:

1. Установи PostgreSQL
2. Создай базу данных:
   ```sql
   CREATE DATABASE neverlose;
   CREATE USER neverlose WITH PASSWORD 'password';
   GRANT ALL PRIVILEGES ON DATABASE neverlose TO neverlose;
   ```
3. В `.env` раскомментируй:
   ```env
   DATABASE_URL=postgres://neverlose:password@localhost/neverlose
   ```
4. Запусти миграции:
   ```batch
   cd rust-server
   cargo install sqlx-cli
   sqlx migrate run
   ```

**Но это НЕ обязательно!** Сервер работает и без БД в in-memory режиме.

## 🔍 Логи

Сервер выводит логи в консоль. Уровень логирования настраивается в `.env`:

```env
RUST_LOG=info    # trace, debug, info, warn, error
```

## ✅ Проверка работы

1. Запусти `START_SERVER.bat`
2. Увидишь:
   ```
   [INFO] Starting HTTP server on 0.0.0.0:30031
   [INFO] Starting WebSocket server on 0.0.0.0:30030
   ```
3. Открой в браузере: http://127.0.0.1:30031/
4. Если видишь ответ от сервера - всё работает!

## 🐛 Troubleshooting

### Ошибка "Address already in use"
Порт 30030 или 30031 уже занят. Измени порты в `.env` или останови процесс:
```batch
netstat -ano | findstr :30031
taskkill /PID <PID> /F
```

### DLL не подключается к серверу
1. Проверь что сервер запущен
2. Проверь что добавил `162.19.230.28` в hosts
3. Проверь firewall - порты 30030 и 30031 должны быть открыты

### "Database connection failed"
Если не нужна БД, закомментируй `DATABASE_URL` в `.env`

## 📝 Примечания

- Сервер работает **БЕЗ базы данных** по умолчанию
- Auth bypass включен (`AUTH_BYPASS=true`)
- Serial захардкожен из `Requestor.cpp`
- Lua библиотеки можно положить в папку `rust-server/lua/` (будет создана автоматически)
- Конфиги и скрипты хранятся в памяти (если нет БД)

## 🎯 Результат

После запуска сервера и настройки hosts файла:
1. Запусти CS:GO
2. Инжектируй DLL
3. DLL подключится к твоему локальному серверу
4. Всё работает локально без интернета!

---

**Совет:** Добавь `START_SERVER.bat` в автозагрузку, чтобы сервер запускался автоматически при старте Windows.
