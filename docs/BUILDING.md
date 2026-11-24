> Инструкция унаследована от shrekNX и описывает сборку порта Shrek 2. Для Spider-Man 2 PC в этой ветке работает путь `tools/recomp.sh` (генерация C + сборка библиотек + smoke-тест + headless-прогон до кадрового цикла); подробности — [RECOMP_STATUS.md](RECOMP_STATUS.md).
>
> Headless-сборка не требует SDL2/OpenGL/OpenAL/FreeType: `tools/build_headless.sh` использует
> заглушки из `platform/headless/`. Запуск: `tools/recomp.sh headless` (после `gen`/`build`).

# Сборка и запуск

Все команды выполняются из корня репозитория. Проверенная платформа разработки — macOS arm64. Поддержка сборки на Linux добавлена в CMake (`PkgConfig`, OpenGL 4.1 core, `SNX_PIN` доступен на Linux ARM64 GCC; сборка и запуск на реальном Linux — `[проверить]`). Windows как хост пока не поддерживается.

## Зависимости macOS

Нужны Command Line Tools for Xcode (Apple Clang), Python 3.11+, CMake 3.20+, Ninja, SDL2, libogg/libvorbis, FFmpeg и FreeType. OpenAL берётся из системного framework.

Установите недостающие инструменты:

```sh
xcode-select --install
brew install python cmake ninja sdl2 libogg libvorbis ffmpeg freetype
python3 -m venv tools/.venv
tools/.venv/bin/python -m pip install -r tools/requirements.txt
```

CMake ищет библиотеки в `$HOMEBREW_PREFIX`, `/opt/homebrew`, `/usr/local` и через `PkgConfig` (при наличии). Большие загрузки Homebrew могут занять время. Python-зависимости устанавливаются через pip в `tools/.venv`.

## Зависимости Linux (`[проверить]`)

На Ubuntu/Debian требуются компилятор C/C++, CMake, Ninja, `pkg-config`, Python 3.11+ и dev-пакеты библиотек:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl2-dev libgl-dev libopenal-dev libvorbis-dev libogg-dev \
    libavcodec-dev libavutil-dev libfreetype-dev
```

Сборка без игры (на Linux ARM64 с GCC можно дополнительно включить `-DSNX_PIN=ON`):

```sh
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
```

## Проверка без игры

```sh
tools/build.sh mac
ctest --test-dir build/mac --output-on-failure
tools/.venv/bin/python -I tests/test_recomp.py
build/mac/host/hello/snx_hello --frames 60
```

CTest запускает 10 синтетических тестов рантайма и HLE: `rep_test`, `fist_test`, `crand_test`, `vmem_test`, `heap_test`, `d3d8_ps_test`, `wfile_test`, `wfile_no_cache_test`, `bink_test`, `config_test`. `tests/test_recomp.py` проверяет модули рекомпилятора без файлов игры. GL-пример открывает окно и проверяет DXT1; он требует графического сеанса и запускается отдельно от CI.

Сборка и прогон CTest под санитайзерами (ASan + UBSan):

```sh
cmake -S . -B build/mac-san -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DSNX_SANITIZE=address;undefined"
cmake --build build/mac-san --target rep_test fist_test crand_test wfile_test vmem_test heap_test d3d8_ps_test bink_test config_test
ctest --test-dir build/mac-san --output-on-failure
```

Цели `snx_boot` и `difftest_*` появляются только после генерации соответствующих модулей. Если в CMake уже настроен другой каталог сборки, он не подхватывается автоматически: `tools/build.sh mac` использует `build/mac`.

## Подготовка своей игры

Нужна установленная **PC-версия KnowWonder Shrek 2**, а не консольное издание. Поместите её в `game/`:

```text
game/
  System/        game.exe, DLL, пакеты .u, Default.ini, DefUser.ini, локализация
  Maps/          .unr
  Textures/      .utx
  StaticMeshes/  .usx
  Animations/    .ukx
  Sounds/        .uax
  Music/         .ogg
  Movies/        .bik
  Help/
```

Все 12 нативных модулей перечислены в `recomp/addrmap.json`. Проверенная английская сборка использует DLL от 2004-04-21 и распакованный `game.exe` от 2004-04-14. Защищённый SafeDisc `Game.exe` с диска напрямую не подходит: его код зашифрован. Репозиторий не содержит исполняемых файлов игры и не предоставляет способ их получения. Другие издания не считаются совместимыми без проверки.

`game/`, `Shrek2RUS/` и `original/` используются только для чтения. Настройки, логи и сохранения пишутся в отдельный пользовательский каталог.

## Перевод модулей и запуск на macOS

Перед генерацией можно проверить совместимость каталога игры (наличие 12 модулей, отсутствие защиты SafeDisc, совпадение всех 56 сигнатур/экспортов `overrides.py` и каталогов ассетов):

```sh
tools/.venv/bin/python -I recomp/main.py check --game game/System
```

Перевод и запуск (генерация по умолчанию параллельна на всех ядрах; `-j N` / `--jobs N` задаёт число процессов, `--jobs 1` — последовательно):

```sh
tools/.venv/bin/python -I recomp/main.py gen --game game/System --out build/gen
tools/build.sh mac snx_boot
build/mac/host/boot/snx_boot --game game --user build/userdata
```

Генерация и первая сборка занимают несколько минут. `build/gen/` содержит код, полученный из локальной копии игры, и остаётся локальным. При смене исходных DLL или рекомпилятора повторите генерацию и сборку.

Проверка загрузки DLL без входа в игру:

```sh
build/mac/host/boot/snx_boot --game game --user build/userdata-check --dlls-only
```

Короткий запуск первого уровня или пакетный прогон карт (`tools/test_maps.py`):

```sh
build/mac/host/boot/snx_boot --game game --user build/userdata-check --seconds 90 -- 1_Shreks_Swamp.unr
tools/.venv/bin/python -I tools/test_maps.py --boot build/mac/host/boot/snx_boot --game game --user build/test-maps --seconds 20
```

Первый запуск создаёт настройки из файлов игры. Игра видит виртуальный `C:\Shrek2`, а записи попадают в `build/userdata` (или путь из `--user`). Оригиналы не меняются. `--headless` отключает SDL, но сам по себе не выбирает NullRenderDevice: используйте `tools/test_maps.py --headless` или отдельный конфиг из [AGENTS.md](../AGENTS.md). Разбор лога падения Switch — `tools/probe/crash.py`.

## Nintendo Switch

Установите [devkitPro](https://devkitpro.org/wiki/Getting_Started) и пакеты Switch. Команда требует прав администратора:

```sh
sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-glad switch-libdrm_nouveau switch-openal-soft switch-libvorbis switch-libogg switch-ffmpeg switch-zlib switch-freetype
```

По умолчанию используется `/opt/devkitpro`; для другого каталога задайте `DEVKITPRO`. После генерации английских модулей:

```sh
tools/build.sh switch snx_boot_nro
```

Скопируйте локально собранный NRO и данные на SD:

```text
sdmc:/switch/shrek2/snx_boot.nro
sdmc:/switch/shrek2/game/System/...
sdmc:/switch/shrek2/game/Maps/...
```

Запускайте hbmenu в **title override**: удерживайте R при запуске установленной игры. Applet-режима через Album недостаточно для окна гостевой памяти в 1 ГБ. Консоль должна быть настроена для запуска homebrew.

Настройки и сохранения создаются в `sdmc:/switch/shrek2/user/`, лог без nxlink — в `user/snx.log`, лог движка — в `user/System/game.log`. Разрешение по умолчанию: 1280×720 в руках, 1920×1080 в доке. Управление и `Snx.ini` описаны в [CONSOLE.md](CONSOLE.md).

Для запуска через сеть включите netloader кнопкой Y в hbmenu:

```sh
/opt/devkitpro/tools/bin/nxlink -s build/switch/host/boot/snx_boot.nro
```

Заливка данных через ftpd, если он уже запущен на Switch:

```sh
tools/.venv/bin/python -I tools/switch_upload.py --host <SWITCH_IP> game /switch/shrek2/game
```

Иконка NRO создаётся локально из `original/` или `game/Help/Logo.bmp`, если эти файлы есть; иначе используется стандартная иконка libnx. Она не добавляется в репозиторий.

PGO — отдельный необязательный этап: [AGENTS.md](../AGENTS.md), раздел «Switch». `tools/build.sh switch` использует локальный `build/pgo`, когда найден `stamp`; `SNX_PGO= tools/build.sh switch` отключает его. Профиль и `build/hot_order.ld` не распространяются вместе с исходниками.

## Android

Отдельная инструкция: [ANDROID.md](ANDROID.md). Кратко — после генерации модулей:

```sh
tools/android_deps.sh --ffmpeg
tools/build.sh android        # или android-rus
```

## Русская версия

В Spider-Man 2 PC русская локализация — это `.int`-файлы в `System` (например, `Startup.int` в UTF-16
с кириллицей), а PE-модули (`Webhead.exe`, `Core.dll` и т. д.) те же, что и в английской копии. Отдельной
генерации `build/gen-rus` не требуется: `cmake/gen.cmake` при `SNX_VARIANT=rus` берёт общий `build/gen`.
Язык определяется по содержимому `.int`, настройки и сохранения пишутся в отдельный каталог `user-rus`.

```sh
tools/build.sh android-rus       # -> build/webheadNX-rus.apk (или mac-rus / switch-rus)
```

Вариант APK (`eng` или `rus`) выбирается при сборке; внутри лаунчер определяет язык по `System/Startup.int`,
поскольку CRC и дата модулей у локализаций одинаковые.

## Частые проблемы

| Симптом | Проверка |
|---|---|
| Нет цели `snx_boot` | Сначала выполните `recomp ... gen`, затем снова `tools/build.sh` |
| Ошибка CRC модуля | NRO и DLL должны быть из одной копии игры; перегенерируйте код и пересоберите |
| Не найден `SDL2` / FFmpeg / FreeType | Проверьте зависимости Homebrew и архитектуру arm64 |
| Ошибка выделения памяти на Switch | Запускайте через title override |
| Ловушка HLE / неизвестный адрес | Сохраните адрес, имя модуля и небольшой фрагмент лога; это может быть неподдерживаемый путь или другая сборка игры |

Дифф-тесты с Unicorn требуют локальных файлов игры и описаны в [AGENTS.md](../AGENTS.md). Они не запускаются публичным CI.
