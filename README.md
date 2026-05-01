
# build instructions

```bash
msbuild neverlose.sln /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v145
```

## notes

* the toolset might be different on your system
* sometimes you don’t need to specify it at all
* if it fails, just change/remove the toolset in the project settings


## server notes

* make sure your DB is set up (`DATABASE_URL`, etc)
* run migrations before starting the server
* if it doesn’t work first try, it’s usually just an env/toolchain mismatch



## local server setup

🚀 **Сервер уже готов!** Просто запусти:

1. `server/SETUP_HOSTS.bat` (от админа) - настроит hosts файл
2. `server/START_SERVER.bat` - запустит сервер
3. Инжектируй DLL в CS:GO

Подробнее: `server/QUICK_START.txt` или `server/SERVER_SETUP.md`

**Что работает:**
- ✅ HTTP API (port 30031) - Lua libraries, configs
- ✅ WebSocket API (port 30030) - live updates
- ✅ Auth bypass - не требуется токен
- ✅ In-memory storage - без базы данных

## last thing

this whole project — especially the server — has been a massive pain to get working properly

me and spiny nuggie have probably lost a few years of our lives fixing this thing, but it's finally in a usable state

[http://162.19.230.28:25578/](http://162.19.230.28:25578/)

thanks for hitting a peak of **1.6k users** ❤️
means a lot
