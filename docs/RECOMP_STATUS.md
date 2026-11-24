# Состояние рекомпиляции Spider-Man 2 PC

Документ описывает фактический результат прогона рекомпилятора на копии игры из
`references/coolgame` (dll от 2004-06-03…2004-06-10, `Webhead.exe` без `.reloc`, база `0x400000`).

## Карта модулей

Адреса в `recomp/addrmap.json`. Важное отличие от Shrek 2: `Webhead.exe` собран без
релокаций, поэтому он остаётся на своей предпочтительной базе `0x00400000` (в shrekNX
исполняемый файл игры переносился на `0x10900000`). Остальные 11 модулей ребейзятся
в окно `0x10000000…0x40000000`.

| Модуль | Гостевой адрес | Функций | C-файлов | Ошибок трансляции | Комментарий |
|---|---:|---:|---:|---:|---|
| msvcr71.dll | 0x13000000 | 1 816 | 8 | 118 | ожидаемо: x87-код CRT; все математические экспорты заменены `ovr_crt_*` |
| Core.dll | 0x10100000 | 8 228 | 29 | 0 | |
| Fire.dll | 0x10500000 | 678 | 4 | 0 | |
| Webhead.dll | 0x10700000 | 4 902 | 18 | 0 | |
| Webhead.exe | 0x00400000 | 1 469 | 6 | 0 | без релокаций |
| Window.dll | 0x11000000 | 4 345 | 16 | 0 | |
| WinDrv.dll | 0x11100000 | 1 765 | 7 | 0 | |
| Engine.dll | 0x12000000 | 31 886 | 108 | 0 | самый крупный модуль |
| D3DDrv.dll | 0x12800000 | 3 234 | 12 | 0 | |
| GUI.dll | 0x12A00000 | 3 130 | 12 | 0 | |
| Gameplay.dll | 0x12B00000 | 219 | 2 | 0 | |
| ALAudio.dll | 0x12C00000 | 675 | 4 | 0 | |
| **Итого** | | **62 347** | **227** | **118 (только CRT)** | |

## Проверки

1. `python3 -I recomp/main.py check --game references/coolgame/System` — модули 12/12,
   сигнатуры и экспорты 43/43, каталоги ассетов 5/5, ошибок 0.
2. `tools/recomp.sh gen` — discovery 65 с, сводки x87 14,5 с, регистры 12,7 с, генерация 16 с
   (2 потока); 130 MiB сгенерированного C.
3. `tools/compile_gen.py` — 227/227 C-файлов собраны в `.o`, 13 архивов в `build/lib`.
4. `build/smoke/spider_smoke` — 12 модулей загружены, вызовы `Core!appStrlen` и
   `Core!appMemcmp` вернули корректные значения. `SMOKE OK`.
5. Headless-experiment: `rt_module_attach()` для всех 12 модулей завершается успешно
   (с заглушками GL/FreeType/OpenAL, которые в репозиторий не включены).

## Что НЕ сделано

Это **рекомпиляция кода игры**, а не готовый порт. До запуска игры осталось:

- **HLE под Webhead.** В слое совместимости унаследованы Shrek-специфичные правки
  (скрипт `SHGame`, HUD, D3D-состояния). Они отключены: `recomp/overrides.py` содержит
  только замены CRT и `memcpy`. Добавлены два общих вызова, нужных Webhead:
  `user32!FindWindowExA/W` и `kernel32!GetPrivateProfileStringA/W`.
  Дальнейшие ловушки headless-запуска: `gdi32!CreateDIBitmap`, затем, вероятно, D3D8/OpenAL.
- **Хостовая графика и звук.** `runtime/hle/d3d8*.c`, `platform/window.c`, `overlay.c`
  рассчитаны на SDL2/OpenGL 4.1/OpenAL/FreeType. В песочнице этих dev-пакетов нет,
  поэтому `snx_boot` здесь не собирается; собирается только «чистая» часть (код игры
  и рантайм без HLE-графики).
- **Игровая конфигурация.** `host/boot/config.c` теперь работает с `System/Webhead.ini`
  вместо `game.ini`, guest root — `C:\\Spider-Man2`. Привязки геймпада и читы
  унаследованы от Shrek 2 и требуют проверки в Spider-Man 2.
- Switch/Android-сборки не собирались (нет devkitPro/Android SDK).

## Воспроизведение

```sh
tools/recomp.sh all      # check + gen + build + smoke
```

Результат: `build/gen/*.c`, `build/lib/lib*.a`, `build/REPORT.md`, `SMOKE OK` в конце.

## Источники

- `https://github.com/Player124413/Shrek2-android_edition` — рекомпилятор, HLE, рантайм (MIT).
- `https://github.com/Player124413/coolgame` — копия PC-игры: PE-модули и ассеты (не коммитятся).

Оба репозитория клонированы полностью (`--no-single-branch`: все ветки, теги и история).
