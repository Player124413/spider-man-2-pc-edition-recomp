# webheadNX — рекомпиляция Spider-Man 2 (PC, 2004)

Проект переводит нативные x86-модули **Spider-Man 2 для ПК** (движок Webhead / Unreal Engine 2)
в C и собирает их в статические библиотеки. Инструментарий и слой совместимости взяты из
[shrekNX](https://github.com/Player124413/Shrek2-android_edition) — рекомпилятора и HLE для
очень похожего PC-движка (KnowWonder Shrek 2), и адаптированы под Webhead.

Исходные файлы игры **не входят** в этот репозиторий. Для рекомпиляции нужна ваша собственная
совместимая копия Spider-Man 2 PC; в рабочем каталоге она клонирована в `references/coolgame`
(каталог исключён из git, как и весь `references/`).

## Что уже работает

- `recomp/` — проверенный на этой сборке игры рекомпилятор x86 → C (12 модулей, 62 347 функций).
- Все переведённые модули компилируются GCC: 227 C-файлов → 13 архивов в `build/lib`.
- `tests/smoke/spider_smoke.c` грузит все 12 модулей в гостевую память и выполняет реальный
  сгенерированный код из `Core.dll` (`appStrlen`, `appMemcmp`) — тест проходит.
- `DllMain` всех модулей успешно вызывается в headless-проверке.
- Полностью игровой запуск (графика, звук, ролики) **пока не готов**: слой HLE требует
  доработки под Webhead, а в песочнице нет SDL2/OpenGL/OpenAL/FreeType. Подробности —
  [docs/RECOMP_STATUS.md](docs/RECOMP_STATUS.md).

## Быстрый старт

```sh
# 1. Зависимости Python (создаётся .venv автоматически)
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt

# 2. Полные клоны исходников (уже сделаны в этом рабочем каталоге)
git clone --no-single-branch https://github.com/Player124413/Shrek2-android_edition references/Shrek2-android_edition
git clone --no-single-branch https://github.com/Player124413/coolgame references/coolgame

# 3. Проверка игры, трансляция, сборка и smoke-тест
tools/recomp.sh all
```

Переменные окружения: `GAME` (путь к каталогу игры, по умолчанию `references/coolgame`),
`JOBS` (число параллельных процессов), `GEN` (каталог сгенерированного C), `CC`.

Отдельные шаги: `tools/recomp.sh check`, `tools/recomp.sh gen`, `tools/recomp.sh build`,
`tools/recomp.sh smoke`. Generated C и объекты попадают в `build/` и в git не добавляются.

## Структура

| Каталог | Назначение |
|---|---|
| `recomp/` | x86-дизассемблер, discovery, генерация C, таблицы модулей, карта адресов |
| `runtime/` | гостевая память, загрузчик PE, вызовы гостя, замены CRT, HLE Win32/D3D8 |
| `host/`, `platform/` | хостовой загрузчик и SDL/OpenGL-слой (унаследованы от shrekNX, требуют доработки) |
| `tools/` | `recomp.sh`, `compile_gen.py`, `build_smoke.sh` и утилиты shrekNX |
| `tests/smoke/` | проверка сгенерированного кода без графики |
| `references/` | полные клоны репозиториев-источников (не коммитятся) |

## Лицензия и активы

Код рекомпилятора, рантайма и HLE — MIT, copyright (c) 2026 Ananasiy
([LICENSE.shreknx](LICENSE.shreknx), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
Файлы Spider-Man 2 являются собственностью правообладателей и не распространяются этим проектом.
