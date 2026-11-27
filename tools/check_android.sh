#!/usr/bin/env bash
# Проверка Android-проекта без Android SDK: состав файлов, соответствие карте модулей и отсутствие
# Shrek-специфичных путей. Полная сборка APK требует JDK 17 + SDK/NDK (docs/ANDROID.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

check "Gradle-проект" "test -f '$ROOT/android/app/build.gradle' && test -f '$ROOT/android/settings.gradle'"
check "Kotlin-лаунчер и игра" "test -f '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt' && test -f '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt'"
check "JNI-мост" "test -f '$ROOT/host/boot/android.c' && grep -q 'Java_com_webheadnx_game_SnxNative' '$ROOT/host/boot/android.c'"
check "режим выбора пунктов меню" "grep -q 'touch_menu_override' '$ROOT/host/boot/input.c' && grep -q 'JNI_FN(menuMode)' '$ROOT/host/boot/android.c' && grep -q 'fun menuMode' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt'"
check "BinkWait в HLE" "grep -q 'hle_binkw32__BinkWait_4' '$ROOT/runtime/hle/bink.c' && ! grep -q 'BinkWait' '$ROOT/platform/headless/stubs.c'"
check "меню только на заставке" "grep -q 'bink_menu_movie_active' '$ROOT/runtime/hle/bink.c' && grep -q 'bink_menu_movie_active()' '$ROOT/host/boot/input.c' && grep -q 'Marvel.bik' '$ROOT/runtime/hle/bink.c'"
check "кнопки в главном меню" "grep -q 'STATE_MENU_PASS' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt' && grep -q 'passThroughMenu' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt' && grep -q 'controlsVisible' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "тапы в меню через SDL-мышь" "grep -qF 'evs[i].down == 2' '$ROOT/host/boot/input.c' && grep -q 'touch(u, v)' '$ROOT/host/boot/input.c' && grep -q 'game_menu_cursor(1)' '$ROOT/host/boot/input.c' && grep -q 'SnxNative.mouse(x / max(1f, w()), y / max(1f, h()), 2)' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "ввод в игре не зависит от защёлки меню" "grep -q 'int frozen = gui_menu || g_menu_override' '$ROOT/host/boot/input.c' && grep -q 'if (!g_input_frozen) {' '$ROOT/host/boot/input.c' && grep -q 'if (!g_input_frozen) {' '$ROOT/host/boot/input.c'"
check "тап в мышь только вне игры" "grep -q 'g_touch_in_game = game_player_active()' '$ROOT/host/boot/input.c' && grep -q '(g_touch_in_game ? 8 : 0)' '$ROOT/host/boot/input.c' && grep -q 'STATE_IN_GAME = 8' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt' && grep -q 'menuOverride || !inGame' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "порог тапа 24 dp" "grep -qF '24f * density' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "настройки камеры в шестерёнке" "grep -q 'settingsMenu()' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt' && grep -q 'Чувствительность камеры' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt'"
check "specular-туман без specular — без тумана" "grep -qF '(k->inputs & (1u << 6)) ? \"v_col1.a\" : \"1.0\"' '$ROOT/runtime/hle/d3d8_ff.c'"
check "замеры картинки в логе" "grep -q 'g_diag_variant' '$ROOT/runtime/hle/d3d8_ff.c' && grep -qF '[diag] pic' '$ROOT/runtime/hle/d3d8_ff.c' && grep -qF '[diag] сетка' '$ROOT/runtime/hle/d3d8_ff.c' && grep -q 'x_diag_frame' '$ROOT/runtime/hle/d3d8_ff.c' && grep -qF 'webheadNX build' '$ROOT/host/boot/main.c' && grep -q 'SNX_DIAG_AUTO' '$ROOT/runtime/hle/d3d8_ff.c' && grep -q 'u_nogam' '$ROOT/runtime/hle/d3d8_ff.c'"
check "небо как в оригинале: туман выключен, переключателя нет" "grep -q 'static int fog_on' '$ROOT/runtime/hle/d3d8_ff.c' && grep -q 'rs\[28\] && fog_on()' '$ROOT/runtime/hle/d3d8_ff.c' && grep -q 'SNX_FOG' '$ROOT/runtime/hle/d3d8_ff.c' && ! grep -q 'SNX_FOG' '$ROOT/android/app/src/main/java/com/webheadnx/game/Settings.kt' && ! grep -qF 'Туман как в игре' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt' && ! grep -qF 'Туман как в игре' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt'"
check "диагностика кадра убрана из интерфейса" "grep -q 'int d3d8_diag' '$ROOT/runtime/hle/d3d8_ff.c' && grep -q 'SNX_DIAG' '$ROOT/runtime/hle/d3d8_ff.c' && ! grep -qF 'Диагностика кадра' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt' && ! grep -q 'external fun diag' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt' && ! grep -q 'JNI_FN(diag)' '$ROOT/host/boot/android.c'"
check "камера в игре — осью, мышь только меню и паузе" "grep -q 'static int g_cam_mouse;' '$ROOT/host/boot/input.c' && grep -qF 'CameraMode=Joystick' '$ROOT/host/boot/input.c' && grep -q 'padCamera2' '$ROOT/android/app/src/main/java/com/webheadnx/game/Settings.kt' && grep -q '(g_invert_x ? -rx : rx) \* cs' '$ROOT/host/boot/input.c'"
check "камера пальцем — как мышь ПК: сколько провёл, на столько и повернулось (без стика)" "grep -q 'void touch_look' '$ROOT/host/boot/input.c' && grep -q 'JNI_FN(look)' '$ROOT/host/boot/android.c' && grep -q 'external fun look(' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt' && grep -qF 'SnxNative.look((x - f.lastX) / max(1f, h())' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt' && ! grep -q 'lookRadius()' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt' && grep -q 'dinput8_mouse_move(dx, dy, 0)' '$ROOT/host/boot/input.c'"
check "камера пальцем: постоянный коэффициент, без автоподкруток" "grep -q 'define CAM_COUNTS_PER_HEIGHT 1000.0f' '$ROOT/host/boot/input.c' && grep -q 'define CAM_COUNTS_PER_HEIGHT_P 667.0f' '$ROOT/host/boot/input.c' && ! grep -q 'cam_tick' '$ROOT/host/boot/input.c'"
check "в геймплее мышь выключена, в меню работает" "grep -q 'static int mouse_allowed(void)' '$ROOT/host/boot/input.c' && grep -q 'if (!mouse_allowed())' '$ROOT/host/boot/input.c' && grep -q 'return g_menu || !game_player_active();' '$ROOT/host/boot/input.c'"
check "настройка камеры в шестерёнке действует на ходу" "grep -q 'void input_set_camera' '$ROOT/host/boot/input.c' && grep -q 'JNI_FN(camSet)' '$ROOT/host/boot/android.c' && grep -q 'external fun camSet' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt' && grep -qF 'Мёртвая зона стика' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt' && grep -qF 'Зеркалить поворот' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt' && grep -q 'SNX_CAM_DEAD' '$ROOT/android/app/src/main/java/com/webheadnx/game/Settings.kt'"
check "установка APK — браузером, без REQUEST_INSTALL_PACKAGES" "! grep -q 'android.permission.REQUEST_INSTALL_PACKAGES' '$ROOT/android/app/src/main/AndroidManifest.xml' && grep -q 'openInBrowser' '$ROOT/android/app/src/main/java/com/webheadnx/game/Updater.kt' && ! grep -q 'ApkProvider' '$ROOT/android/app/src/main/AndroidManifest.xml'"
check "чувствительность камеры действует на ходу" "grep -q 'void input_set_look_sens' '$ROOT/host/boot/input.c' && grep -q 'JNI_FN(lookSens)' '$ROOT/host/boot/android.c' && grep -q 'external fun lookSens' '$ROOT/android/app/src/main/java/com/webheadnx/game/SnxNative.kt' && grep -q 'SnxNative.lookSens' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameActivity.kt' && grep -q 'powf(k, 1.4f)' '$ROOT/host/boot/input.c'"
check "texgen SPHEREMAP" "grep -q 'SPHEREMAP' '$ROOT/runtime/hle/d3d8_ff.c' && grep -q '(t\[11\] >> 16) & 7' '$ROOT/runtime/hle/d3d8_ff.c'"
check "ролики растянуты на весь кадр" "grep -q 'glBlitFramebuffer(0, 0, mw, mh, 0, 0, W, H' '$ROOT/runtime/hle/d3d8_res.c' && grep -q 'glBlitFramebuffer(0, 0, (GLint)q->w, (GLint)q->h, 0, 0, W, H' '$ROOT/runtime/hle/d3d8_ff.c'"
check "защита FArray::Remove от GPF" "grep -q 'ovr_core_farray_remove' '$ROOT/runtime/overrides_core.c' && grep -qF 'Core.dll!?Remove@?\$_FArray@X@@QAEXHHH@Z' '$ROOT/recomp/overrides.py'"
check "мышь по умолчанию, без кнопки" "! grep -q 'Control(\"mouse\"' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchLayout.kt' && grep -q 'drawMouseCursor' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt' && grep -q 'SnxNative.mouse(x / max(1f, w()), y / max(1f, h()), -1)' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "ручной режим меню по кнопке Меню" "grep -q 'menuOverride = !menuOverride' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt' && grep -q 'SnxNative.menuMode(menuOverride)' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "клавиатура в DirectInput" "grep -q 'void dinput8_key' '$ROOT/runtime/hle/dinput8.c' && grep -q 'dinput8_key((int)vk, down)' '$ROOT/host/boot/input.c' && grep -q 'DEV_KEYBOARD && cb >= 256' '$ROOT/runtime/hle/dinput8.c'"
check "защёлка меню держится до выхода" "grep -q 'if (game_player_active())' '$ROOT/host/boot/input.c' && ! grep -q 'g_menu_clicked && d3d8_frame_draws()' '$ROOT/host/boot/input.c' && grep -q 'g_menu_latch = 0' '$ROOT/host/boot/input.c'"
check "перед/назад дублируется W/S" "grep -q 'vks.add(0x57)' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt' && grep -q 'vks.add(0x53)' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchControlsView.kt'"
check "раскладка: без зелий, есть пропуск" "grep -q 'VERSION = 9' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchLayout.kt' && ! grep -q 'Control(\"enter\"' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchLayout.kt' && grep -q 'Пропуск' '$ROOT/android/app/src/main/java/com/webheadnx/game/TouchLayout.kt'"
check "лаунчер без выбора уровня и читов" "! grep -q 'EXTRA_LEVEL' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt' && ! grep -q 'Level select' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt' && ! grep -q 'Уровни, сохранения, читы' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt' && ! grep -q 'Читы в игре' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt' && ! grep -q 'Level select' '$ROOT/android/app/src/main/java/com/webheadnx/game/Cheats.kt' && grep -q 'Импорт сохранений…' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt'"
check "импорт сохранений" "grep -q 'Saves.import' '$ROOT/android/app/src/main/java/com/webheadnx/game/LauncherActivity.kt' && grep -q 'fun import' '$ROOT/android/app/src/main/java/com/webheadnx/game/Saves.kt'"
check "CMake Android" "test -f '$ROOT/cmake/android.cmake' && grep -q 'SNX_ANDROID_DEPS' '$ROOT/cmake/android.cmake'"
check "зависимости" "test -x '$ROOT/tools/android_deps.sh' && grep -q 'ffmpeg-src' '$ROOT/tools/android_deps.sh'"
check "Workflow" "test -f '$ROOT/.github/workflows/android.yml' && grep -q 'SNX_ANDROID_VIDEO' '$ROOT/.github/workflows/android.yml'"
check "отдельный ID приложения" "grep -q \"applicationId 'com.webheadnx.game'\" '$ROOT/android/app/build.gradle' && ! grep -q \"applicationId 'com.shreknx.game'\" '$ROOT/android/app/build.gradle'"
check "отдельный ключ подписи" "test -f '$ROOT/android/webhead2-release.p12.b64' && grep -q 'webhead2-release.p12.b64' '$ROOT/android/app/build.gradle' && grep -q 'keyAlias.*webhead2' '$ROOT/android/app/build.gradle'"
check "подпись v1+v2+v3" "grep -q 'enableV1Signing true' '$ROOT/android/app/build.gradle' && grep -q 'enableV2Signing true' '$ROOT/android/app/build.gradle' && grep -q 'enableV3Signing true' '$ROOT/android/app/build.gradle' && grep -q 'Verified using v3 scheme' '$ROOT/.github/workflows/android.yml'"
check "окно «О порте» в цветах паука, без ссылки на репозиторий" "! grep -q 'Огромное спасибо' '$ROOT/android/app/src/main/java/com/webheadnx/game/CreditsDialog.kt' && ! grep -q 'L.UPSTREAM' '$ROOT/android/app/src/main/java/com/webheadnx/game/CreditsDialog.kt' && grep -q '0xFF8C1319' '$ROOT/android/app/src/main/java/com/webheadnx/game/CreditsDialog.kt' && grep -q '0xFFFF5B60' '$ROOT/android/app/src/main/java/com/webheadnx/game/CreditsDialog.kt'"
check "Spider-Man иконка" "grep -q 'FF2451A8' '$ROOT/android/app/src/main/res/drawable/ic_launcher_foreground.xml' && grep -q 'FFE62429' '$ROOT/android/app/src/main/res/values/colors.xml'"
check "имя exe в лаунчере" "grep -q '\"Webhead.exe\"' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameFolder.kt'"
check "нет game.exe в исходниках порта" "! grep -rq 'game.exe' '$ROOT/android/app/src/main/java' '$ROOT/tools/fetch_game.sh'"
check "fetch_game: PE по имени из addrmap" "grep -q 'exe_dst = next' '$ROOT/tools/fetch_game.sh' && ! grep -q 'dst, \"Webhead.exe\"' '$ROOT/tools/fetch_game.sh'"
check "диагностика Git LFS в роликах" "grep -q 'git-lfs.github.com/spec/v1' '$ROOT/tools/fetch_game.sh' && grep -q 'movieProblem' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameFolder.kt'"

# Один APK на обе локализации: никаких eng/rus/both/uni во workflow, Gradle и скрипте сборки
check "одна ссылка на игру в workflow" "grep -q 'game_url:' '$ROOT/.github/workflows/android.yml' && ! grep -q '^ *variant:' '$ROOT/.github/workflows/android.yml'"
check "нет вариантов eng/rus/uni в workflow" "! grep -Eq '\[eng, rus\]|game_url_rus|both|android-uni' '$ROOT/.github/workflows/android.yml'"
check "нет flavor'ов в Gradle" "! grep -q 'productFlavors\|flavorDimensions' '$ROOT/android/app/build.gradle'"
check "имя приложения в основных ресурсах" "grep -q 'name=\"app_name\"' '$ROOT/android/app/src/main/res/values/strings.xml'"
check "один APK в tools/build.sh" "grep -q 'assembleRelease' '$ROOT/tools/build.sh' && grep -q 'webheadNX.apk' '$ROOT/tools/build.sh'"
check "язык определяется лаунчером" "grep -q 'detectLanguage' '$ROOT/android/app/src/main/java/com/webheadnx/game/GameVariant.kt' && ! grep -rq 'BuildConfig.VARIANT' '$ROOT/android/app/src/main/java'"
check "обновление без вариантов" "grep -q 'webheadNX-nogame-' '$ROOT/android/app/src/main/java/com/webheadnx/game/Updater.kt' && ! grep -q 'variants()' '$ROOT/android/app/src/main/java/com/webheadnx/game/Updater.kt'"

n=$("$ROOT/.venv/bin/python" -c 'import json; print(len(json.load(open("recomp/addrmap.json"))["modules"]))' 2>/dev/null || echo 0)
check "12 модулей в addrmap.json" "test \"$n\" -eq 12"

if [ -x "$ROOT/.venv/bin/python" ]; then
  "$ROOT/.venv/bin/python" - <<'PY' || fail=1
try:
    import yaml
except ImportError:
    print("skip workflow YAML (нет PyYAML)")
else:
    yaml.safe_load(open(".github/workflows/android.yml"))
    print("ok   workflow YAML")
PY
fi

if [ "$fail" -ne 0 ]; then echo "ANDROID CHECK FAIL" >&2; exit 1; fi
echo "ANDROID CHECK OK"
