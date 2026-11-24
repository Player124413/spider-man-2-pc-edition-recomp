# Сторонние зависимости и материалы

Этот репозиторий содержит исходники порта и инструменты; собственный код распространяется по лицензии MIT (`LICENSE`). Зависимости устанавливаются отдельно; их лицензии не заменяются лицензией проекта.

| Компонент | Использование | Лицензия / источник |
|---|---|---|
| SDL2 | Окно, ввод, контекст GL | zlib — https://www.libsdl.org/ |
| Mesa | OpenGL на Switch | Несколько лицензий, преимущественно MIT — https://docs.mesa3d.org/license.html |
| libnx | Системные функции Switch | ISC — https://github.com/switchbrew/libnx |
| devkitA64 / GCC | Компилятор и библиотеки поддержки | GPL с применимыми исключениями для runtime — https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html |
| glad | Загрузчик OpenGL на Switch | Лицензии зависят от генератора и спецификаций Khronos — https://github.com/Dav1dde/glad |
| OpenAL Soft | Звук на Switch и Android (на Android — статически, нужна возможность перелинковки по LGPL) | LGPL — https://openal-soft.org/ |
| OpenAL.framework | Звук на macOS | Системный компонент Apple SDK |
| libogg / libvorbis | Музыка Ogg Vorbis | BSD — https://xiph.org/licenses/ |
| FFmpeg, libavcodec / libavutil | Декодирование видео Bink | LGPL либо GPL в зависимости от конфигурации — https://ffmpeg.org/legal.html |
| FreeType | Растеризация шрифтов | FreeType License либо GPL — https://freetype.org/license.html |
| Android Gradle Plugin, Kotlin | Сборка APK, стандартная библиотека Kotlin в APK | Apache-2.0 — https://developer.android.com/studio/releases/gradle-plugin, https://kotlinlang.org/ |
| zlib | Зависимости сборки Switch | zlib — https://zlib.net/zlib_license.html |
| pefile | Чтение PE | MIT — https://github.com/erocarrera/pefile |
| Capstone | Декодирование x86 | BSD — https://www.capstone-engine.org/ |
| Unicorn | Эталон для дифф-тестов | GPL-2.0 — https://www.unicorn-engine.org/ |
| olefile | Чтение MSI/OLE | BSD — https://github.com/decalage2/olefile |
| Pillow | Контактные листы снимков | HPND — https://python-pillow.github.io/ |

Точный набор лицензий определяется версиями и конфигурацией установленных пакетов. Для распространения связанных бинарников нужно отдельно проверить их лицензии, включая условия статической линковки LGPL-библиотек и конфигурацию FFmpeg. Публичная версия проекта распространяет только исходники порта и инструменты.

Названия Shrek, Shrek 2, Unreal Engine и Nintendo Switch, игровые бинарники, пакеты, графика, звук, шрифты и обложки принадлежат соответствующим правообладателям. Они не входят в лицензию собственного кода проекта.

В качестве технических ориентиров в `AGENTS.md` названы Wine, DXVK, M-HT/SR, xboxrecomp, XenonRecomp и N64Recomp. Это ссылки на подходы и семантику API, а не включённые зависимости. При добавлении кода из внешнего источника укажите конкретный источник и сохраните его лицензионные уведомления.
