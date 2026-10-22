<p align="center">
  <img src="assets/termtalk.svg" alt="TermTalk — Messages. Files. Terminal." width="680">
</p>

# TermTalk

[![Windows CI](https://github.com/Techell/TermTalk/actions/workflows/windows.yml/badge.svg)](https://github.com/Techell/TermTalk/actions/workflows/windows.yml)

[English](#english) · [Русский](#russian)

<a id="english"></a>

A terminal messenger for Windows, written in C. Private conversations, file transfers
and a keyboard-driven interface — with a small, modular client/server core.

**Windows 10/11 · C11 · TCP · English / Русский**

> Early development. Traffic and local history are currently unencrypted.
> Use with trusted peers on trusted networks. See [security](SECURITY.md).

## Features

- Private messaging through a central server; Unicode nicknames and messages.
- Console interface with contacts, command suggestions, Tab completion and 20-command history.
- Saved servers, connection status, round-trip latency and reconnect without restarting.
- Nickname editing before connecting and after a rejected nickname: **F7** in the server menu.
- File offers with explicit accept/reject, progress, SHA-256 checking and a single-pane file browser.
- Optional local conversation history; English and Russian interface switching at any time.
- Configurable server capacity: 1–32 participants; default file limit: 32 GiB per transfer.

Group chats, offline delivery and encrypted connections are planned, not implemented.

## Build

Requires Windows, CMake 3.16+ and a C11 compiler with the Windows SDK.
The locally verified toolchain is MinGW GCC 6.3.0; MSVC is not yet verified.
No third-party runtime libraries are required.

With MinGW available in PATH, run from PowerShell:

```powershell
.\comp.bat
```

This configures and builds into `build/`. The script uses the Windows short path
for compatibility with older MinGW Make versions. On an ASCII path, the equivalent is:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Quick start

Start a server:

```powershell
.\build\server.exe 5050
```

Start the client in a second terminal:

```powershell
.\build\client.exe
```

Choose `127.0.0.1:5050` or enter a server address. Set your nickname using **F7**
or the nickname prompt. A rejected nickname can be changed in the same menu.
Run another client with a different nickname, select a contact with **Tab → arrows → Enter**,
and type a message. `/help` lists commands; **F2** switches EN/RU.

You can also provide the address and nickname directly:

```powershell
.\build\client.exe 127.0.0.1 5050 Alice --lang en
.\build\server.exe 5050 --max-clients 16 --max-file-gib 32
```

Use `/sendfile` or **F6** to select a file, `/accept` to choose a destination,
and `/reject` to decline. **F4** leaves the server; **F3** opens the server menu.
Full commands, storage paths and limits: [user guide](docs/UI.md) (Russian).

## Tests

Python 3 is optional for building the applications, but required for the four
integration tests. Install it before configuring CMake to run the full suite:

```powershell
ctest --test-dir build --output-on-failure
```

The suite includes native Windows console checks, a protocol parser, partial I/O,
queue pressure, reconnect and file-transfer scenarios.
[Verification and limitations](docs/BUILD-VERIFICATION.md).

## Project layout

```text
apps/       Client and server entry points
libs/       Transport, protocol, UI, storage and file-transfer modules
assets/     Project branding
tests/     Contract tests and independent protocol clients
tools/     Roadmap tooling
docs/      User guide, architecture and protocol specifications
.github/    CI and contribution templates
```

## Documentation and contributing

Start with the [documentation index](docs/README.md),
[contributing guide](CONTRIBUTING.md) and [roadmap](docs/ROADMAP.md).
The on-wire protocol remains **WSCK v2**; the rename does not change it.
Legacy `%LOCALAPPDATA%/WinSock` storage paths are preserved to retain existing profiles.

Current application version: **0.9.0**.
See [changelog](CHANGELOG.md).
Licensed under the [MIT License](LICENSE), copyright © 2026 Techell.

<a id="russian"></a>

## Русский

**TermTalk** — терминальный мессенджер для Windows, написанный на Си.
Клиенты общаются через центральный сервер: обмениваются личными сообщениями
и файлами, управляя приложением с клавиатуры. Транспорт, протокол, интерфейс,
хранилище истории и передача файлов разделены на самостоятельные модули.

**Windows 10/11 · C11 · TCP · Интерфейс на русском и английском**

### Возможности

- Личные сообщения, никнеймы и текст с поддержкой Unicode, включая кириллицу.
- Список участников, подсказки команд, дополнение по Tab и история последних 20 команд.
- Меню сохранённых серверов, статус подключения, задержка и смена сервера без перезапуска.
- Смена никнейма по **F7** до подключения и после отказа из-за занятого имени.
- Передача файлов с согласием получателя, прогрессом, проверкой SHA-256
  и однопанельным файловым интерфейсом.
- Отключаемая локальная история переписки и переключение языка клавишей **F2**.
- От 1 до 32 участников на сервере; стандартный лимит одного файла — 32 ГиБ.

Проект находится на ранней стадии разработки. Групповые чаты, офлайн-доставка
и шифрование пока не реализованы. Трафик и локальная история передаются/хранятся
в открытом виде; используйте приложение с доверенными участниками в доверенной сети.
SHA-256 проверяет совпадение содержимого файла, но не подлинность собеседника.
Подробнее — [безопасность](SECURITY.md).

### Сборка и первый запуск

Нужны Windows, CMake 3.16+ и компилятор C11 с Windows SDK. Локально проверен
MinGW GCC 6.3.0; автоматическая сборка на GitHub использует MSYS2/MinGW.
MSVC пока не проверен. Сторонние библиотеки для запуска приложений не требуются.

Добавьте MinGW в PATH и выполните в PowerShell из корня проекта:

```powershell
.\comp.bat
```

Приложения появятся в `build/`. Скрипт использует короткий путь Windows
для совместимости со старым MinGW Make. Запустите сервер:

```powershell
.\build\server.exe 5050
```

В другом терминале запустите клиент с русским интерфейсом:

```powershell
.\build\client.exe --lang ru
```

Выберите `127.0.0.1:5050` или введите адрес сервера и задайте никнейм.
Для проверки общения запустите второй клиент с другим именем.
**Tab → стрелки → Enter** выбирает собеседника; `/help` показывает команды.
**F6** или `/sendfile` открывает выбор файла, `/accept` принимает предложение,
`/reject` отклоняет его. **F4** отключает от сервера, **F3** открывает меню серверов.

Для полного набора проверок установите Python 3 до настройки CMake, затем выполните:

```powershell
ctest --test-dir build --output-on-failure
```

Python нужен для интеграционных проверок, Doxygen — только для необязательной
генерации справочника API. Они не требуются для запуска клиента и сервера.

### Документация и лицензия

- [Руководство пользователя](docs/UI.md): команды, клавиши, файлы и история.
- [Индекс документации](docs/README.md): архитектура, протокол и правила разработки.
- [План развития](docs/ROADMAP.md) и [проверки с ограничениями](docs/BUILD-VERIFICATION.md).
- [Участие в разработке](CONTRIBUTING.md) и [история изменений](CHANGELOG.md).

Текущая версия приложения — **0.9.0**, протокол — **WSCK v2**.
Прежние пути `%LOCALAPPDATA%/WinSock` сохранены для совместимости с локальными данными.
Проект распространяется по [лицензии MIT](LICENSE). Copyright © 2026 Techell.
