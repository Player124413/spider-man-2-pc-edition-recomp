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
| msvcr71.dll | 0x13000000 | 1 816 | 8 | 0 | математические экспорты CRT заменены `ovr_crt_*` |
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
| **Итого** | | **62 347** | **227** | **0** | |

## Проверки

1. `python3 -I recomp/main.py check --game references/coolgame/System` — модули 12/12,
   сигнатуры и экспорты 43/43, каталоги ассетов 5/5, ошибок 0.
2. `tools/recomp.sh gen` — discovery 65 с, сводки x87 14,5 с, регистры 12,7 с, генерация 16 с
   (2 потока); 130 MiB сгенерированного C.
3. `tools/compile_gen.py` — 227/227 C-файлов собраны в `.o`, 13 архивов в `build/lib`.
4. `build/smoke/spider_smoke` — 12 модулей загружены, вызовы `Core!appStrlen` и
   `Core!appMemcmp` вернули корректные значения. `SMOKE OK`.
5. **Headless-прогон (`tools/headless_smoke.sh`) проходит.** Все 12 модулей подключаются,
   движок читает `System/Webhead.ini`, проигрывает вступительные Bink-ролики, создаёт
   D3D8-устройство 1280×1024 и выдаёт кадры (`[d3d8] frame N: 13 draws, 7 programs`).
   За 8 секунд — 0 ловушек HLE, 0 ошибок компиляции GLSL. ASan-прогон 60 с также чистый.

### Что починено для headless

- HLE: `kernel32!InterlockedExchange`, `shell32!SHGetSpecialFolderLocation` /
  `SHGetPathFromIDListA/W`, `binkw32!BinkSetSoundSystem` / `BinkSetSoundOnOff`.
- UAF в `kernel32_file.c!find_first` (READ после `close_find`) — исправлен.
- Конфиг `tests/headless/userdata/System/Webhead.ini`: `D3DDrv.D3DRenderDevice` и
  `ALAudio.ALAudioSubsystem`. `Engine.NullAudioDevice` вызывает исключение при старте.
- GL-заглушки `platform/headless/` возвращают валидные объекты, статусы компиляции
  и размеры; без этого D3D8-драйвер не создавал шейдеры.

## Ограничения

Это **headless-рекомпиляция**, а не интерактивный порт:

- Нет SDL-окна, мыши, клавиатуры и геймпада; `platform/headless/stubs.c` даёт заглушки.
- Видео Bink не декодируется (нет libavcodec): кадры роликов чёрные, тайминги и звук работают.
- Звук OpenAL не выводится наружу (headless-заглушка).
- Хостовая сборка `snx_boot` (SDL2/OpenGL/OpenAL/FreeType/FFmpeg) в песочнице не собиралась:
  нет системных dev-пакетов. Switch/Android-сборки тоже не проверялись.

## Воспроизведение

```sh
tools/recomp.sh all      # check + gen + build + smoke + headless-прогон
```

Результат: `build/gen/*.c`, `build/lib/lib*.a`, `build/REPORT.md`, `SMOKE OK` и
`HEADLESS SMOKE OK` в конце. Headless-игра работает бесконечно, поэтому проверка
ограничена `SNX_SMOKE_SECONDS` (по умолчанию 8 с; код 124 от `timeout` — успех).

## Источники

- `https://github.com/Player124413/Shrek2-android_edition` — рекомпилятор, HLE, рантайм (MIT).
- `https://github.com/Player124413/coolgame` — копия PC-игры: PE-модули и ассеты (не коммитятся).

Оба репозитория клонированы полностью (`--no-single-branch`: все ветки, теги и история).
