package com.webheadnx.game

import android.app.AlertDialog
import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.os.Process
import android.view.Display
import android.view.InputDevice
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.RelativeLayout
import android.widget.ScrollView
import org.libsdl.app.SDLActivity

// Игра: SDLActivity (libSDL2.so + libmain.so, SDL_main из host/boot/main.c) в отдельном процессе :game.
// Поверх поверхности SDL — экранное управление (TouchControlsView) и, по шестерёнке, редактор раскладки.
class GameActivity : SDLActivity(), InputManager.InputDeviceListener, DisplayManager.DisplayListener {
    private lateinit var settings: Settings
    private var touch: TouchControlsView? = null
    private var editor: LayoutEditor? = null
    private var inputManager: InputManager? = null
    private var displayManager: DisplayManager? = null
    private val poll = object : Runnable {
        override fun run() {
            val t = touch ?: return
            val st = try { SnxNative.state() } catch (e: UnsatisfiedLinkError) { 0 }
            t.hiddenByMenu = st and SnxNative.STATE_MENU != 0
            // в главном меню на рольке кнопки должны оставаться: именно A/крестовина выбирают пункты
            t.passThroughMenu = st and SnxNative.STATE_MENU_PASS != 0
            // живой герой в мире: в игре палец — камера, в меню/катсцене — виртуальная мышь
            t.inGame = st and SnxNative.STATE_IN_GAME != 0
            t.postDelayed(this, 100)
        }
    }

    // один APK на обе локализации: сгенерированный код общий, язык берётся из System/*.int
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "main")

    override fun getArguments(): Array<String> = intent.getStringArrayExtra(EXTRA_ARGS) ?: Settings(this).gameArgs()

    override fun onCreate(savedInstanceState: Bundle?) {
        settings = Settings(this)
        L.force = settings.langForce  // язык интерфейса из лаунчера (0 — по языку телефона)
        super.onCreate(savedInstanceState)
        if (SDLActivity.mBrokenLibraries || SDLActivity.mLayout == null) return
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // стабильная производительность: частоты ниже пика, но без троттлинга через 5–10 минут
        if (settings.sustained && Build.VERSION.SDK_INT >= 24) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            if (pm.isSustainedPerformanceModeSupported) window.setSustainedPerformanceMode(true)
        }
        val t = TouchControlsView(this, settings, editMode = false)
        t.onGear = { gearMenu() }
        // камера из лаунчера: тем же путём, что и ползунки в шестерёнке (без перезапуска)
        camApply()
        SDLActivity.mLayout.addView(t, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        touch = t
        inputManager = (getSystemService(Context.INPUT_SERVICE) as InputManager).also {
            it.registerInputDeviceListener(this, null)
        }
        updateGamepad()
        t.post(poll)
        pickDisplayMode()
        displayManager = (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).also {
            it.registerDisplayListener(this, null)
        }
        reportDisplay()
    }

    // Режим экрана под ограничение FPS: частота, равная ему (60 Гц при 60 FPS), иначе самая низкая кратная.
    // Без этого на экране 90/120 Гц ограничитель засыпал между обновлениями экрана и кадры шли рывками
    // (8/17/25 мс вперемешку), а при равной частоте кадры ровно отмеряет vsync (platform/window.c plat_set_display).
    // «Нет» (cap 0) — самая высокая частота экрана: иначе система держит приложение на 60 Гц, и без ограничения
    // всё равно было ровно 60 FPS (vsync выключен отдельно: SNX_NOVSYNC, Settings.gameArgs).
    private fun maxMode(): Display.Mode? {
        if (Build.VERSION.SDK_INT < 23) return null
        @Suppress("DEPRECATION")
        val d = windowManager.defaultDisplay ?: return null
        val cur = d.mode
        return d.supportedModes.filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight }
            .maxByOrNull { it.refreshRate }
    }

    private fun pickDisplayMode() {
        val cap = settings.fpsCap
        if (Build.VERSION.SDK_INT < 23) return
        if (cap <= 0) {
            val m = maxMode() ?: return
            val lp = window.attributes
            lp.preferredDisplayModeId = m.modeId
            lp.preferredRefreshRate = m.refreshRate
            window.attributes = lp
            return
        }
        @Suppress("DEPRECATION")
        val d = windowManager.defaultDisplay ?: return
        val cur = d.mode
        fun mult(m: Display.Mode): Int {
            val k = Math.round(m.refreshRate / cap)
            return if (k in 1..4 && Math.abs(m.refreshRate - k * cap) < 1.5f) k else 0
        }
        val best = d.supportedModes
            .filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight && mult(it) > 0 }
            .minByOrNull { it.refreshRate } ?: return
        val lp = window.attributes
        lp.preferredDisplayModeId = best.modeId
        lp.preferredRefreshRate = best.refreshRate
        window.attributes = lp
    }

    // Частота поверхности (Android 11+): системе — что игра идёт ровно cap кадров в секунду
    private fun setSurfaceFrameRate() {
        val cap = settings.fpsCap
        if (Build.VERSION.SDK_INT < 30) return
        try {
            val s = SDLActivity.mSurface?.holder?.surface ?: return
            if (!s.isValid) return
            if (cap > 0) s.setFrameRate(cap.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            else maxMode()?.let { s.setFrameRate(it.refreshRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT) }
        } catch (e: Exception) {
        }
    }

    private fun reportDisplay() {
        @Suppress("DEPRECATION")
        val hz = windowManager.defaultDisplay?.refreshRate ?: return
        try {
            SnxNative.display(hz, settings.fpsCap)
        } catch (e: UnsatisfiedLinkError) {
        }
    }

    override fun onDisplayAdded(displayId: Int) {}
    override fun onDisplayRemoved(displayId: Int) {}
    override fun onDisplayChanged(displayId: Int) {
        if (displayId == Display.DEFAULT_DISPLAY) reportDisplay()
    }

    override fun onDestroy() {
        inputManager?.unregisterInputDeviceListener(this)
        displayManager?.unregisterDisplayListener(this)
        // libmain.so не умеет запускаться второй раз в том же процессе (окно гостя, глобальные состояния):
        // процесс :game завершается вместе с игрой, следующий запуск — с чистого листа. Сразу, до
        // SDLActivity.onDestroy: тот ждёт конца потока SDL без срока, а игра на SDL_QUIT может и не выйти
        if (isFinishing) quitNow()
        super.onDestroy()
    }

    private fun quitNow() {
        try {
            SnxNative.exitNow()  // сбросить записанное игрой (сохранения, User.ini) и выйти
        } catch (e: UnsatisfiedLinkError) {
        }
        Process.killProcess(Process.myPid())
    }

    override fun onPause() {
        touch?.releaseAll()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        // возврат из фона: пальцы, бывшие на экране, забыты; опрос состояния игры — заново (мог остановиться)
        touch?.let {
            it.releaseAll()
            it.removeCallbacks(poll)
            it.post(poll)
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            immersive()
            setSurfaceFrameRate()
            reportDisplay()
        }
    }

    @Suppress("DEPRECATION")
    private fun immersive() {
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
    }

    // шестерёнка: редактор раскладки. Читы из Shrek 2 в Spider-Man 2 не работают — меню убрано.
    private fun gearMenu() {
        val items = mutableListOf(L.t("Редактор управления", "Touch layout"), L.t("Настройки", "Settings"))
        if (settings.cheats) items.add(L.t("Читы", "Cheats"))
        AlertDialog.Builder(this)
            .setTitle(L.t("Меню", "Menu"))
            .setItems(items.toTypedArray()) { _, i ->
                when (i) {
                    0 -> openEditor()
                    1 -> settingsMenu()
                    else -> Cheats.show(this) { touch?.invalidate() }
                }
            }
            .setNegativeButton(L.t("Отмена", "Cancel"), null)
            .show()
    }

    // Настройки на ходу. Камера пальцем одна, без режимов: сколько палец прошёл — на столько камера и повернулась,
    // а «Чувствительность камеры» задаёт, сколько это градусов. Ползунки действуют сразу, без перезапуска.
    private fun settingsMenu() {
        val c = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val pad = Ui.dp(this, 16f)
        c.setPadding(pad, Ui.dp(this, 8f), pad, 0)
        // Единственная настройка камеры: чувствительность, она же «чувствительность мыши» — сколько градусов
        // поворота даёт жест по всей высоте экрана (1.6 → 144°, верх — 5.0 → 450°).
        c.addView(Ui.seekRow(this, L.t("Чувствительность камеры (мышь)", "Camera sensitivity (mouse)"), 49,
            ((settings.lookSensitivity - 0.1f) * 10f).toInt().coerceIn(0, 49),
            { "%.1f".format(0.1f + it / 10f) }) {
            settings.lookSensitivity = 0.1f + it / 10f
            camApply()
        })
        // «Держать угол»: без этого игра сама доворачивает камеру за спину герою — камера «отщёлкивается назад»
        c.addView(Ui.switchRow(this, L.t("Камера держит угол", "Camera holds its angle"),
            L.t("Не даёт камере возвращаться назад после поворота", "Keeps the camera from snapping back after a turn"),
            settings.camHold) {
            settings.camHold = it
            camApply()
        })
        c.addView(Ui.switchRow(this, L.t("Зеркалить поворот (влево/вправо)", "Mirror turn (left/right)"), null,
            settings.camInvX) {
            settings.camInvX = it
            camApply()
        })
        c.addView(Ui.switchRow(this, L.t("Зеркалить наклон (вверх/вниз)", "Mirror tilt (up/down)"), null,
            settings.camInvY) {
            settings.camInvY = it
            camApply()
        })
        c.addView(Ui.seekRow(this, L.t("Прозрачность кнопок", "Button opacity"), 18,
            ((settings.opacity - 0.1f) * 20f).toInt().coerceIn(0, 18),
            { "${((0.1f + it / 20f) * 100).toInt()} %" }) {
            settings.opacity = 0.1f + it / 20f
            touch?.invalidate()
        })
        c.addView(Ui.switchRow(this, L.t("Вибрация кнопок", "Button vibration"), null, settings.haptics) {
            settings.haptics = it
        })
        AlertDialog.Builder(this)
            .setTitle(L.t("Настройки", "Settings"))
            .setView(ScrollView(this).apply { addView(c) })
            .setPositiveButton(L.t("Готово", "Done"), null)
            .show()
    }

    // параметры камеры живьём (чувствительность, мёртвая зона, скорость, зеркало осей) — без перезапуска игры
    private fun camApply() {
        try {
            SnxNative.camMirror(settings.camInvX, settings.camInvY)
            SnxNative.camHold(settings.camHold)
            SnxNative.lookSens(settings.lookSensitivity)
        } catch (e: UnsatisfiedLinkError) { }
    }

    // редактор поверх игры (игра идёт дальше, экранный геймпад отпущен)
    private fun openEditor() {
        if (editor != null) return
        touch?.releaseAll()
        val ed = LayoutEditor(this, settings) { saved ->
            SDLActivity.mLayout.removeView(editor!!.root)
            editor = null
            if (saved) touch?.controls = TouchLayout.load(this)
            touch?.invalidate()
            touch?.visibility = View.VISIBLE
        }
        editor = ed
        touch?.visibility = View.GONE
        ed.field.setBackgroundColor(0x99000000.toInt())
        SDLActivity.mLayout.addView(ed.root, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }

    override fun onBackPressed() {
        val ed = editor
        if (ed != null) {
            SDLActivity.mLayout.removeView(ed.root)
            editor = null
            touch?.visibility = View.VISIBLE
            return
        }
        // «Назад» не закрывает игру молча (несохранённое пропало бы): спросить
        touch?.releaseAll()
        AlertDialog.Builder(this)
            .setTitle(L.t("Выйти из игры?", "Quit the game?"))
            .setMessage(L.t("Несохранённый прогресс пропадёт. Меню игры — кнопка «Меню».",
                "Unsaved progress will be lost. The game menu is the “Menu” button."))
            .setPositiveButton(L.t("Выйти", "Quit")) { _, _ -> quitNow() }
            .setNegativeButton(L.t("Отмена", "Cancel"), null)
            .setOnDismissListener { immersive() }
            .show()
    }

    // ---------------------------------------------------------------- геймпад: экранное управление прячется
    private fun updateGamepad() {
        var pad = false
        for (id in InputDevice.getDeviceIds()) {
            val d = InputDevice.getDevice(id) ?: continue
            val s = d.sources
            if (!d.isVirtual && (s and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                    s and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)) pad = true
        }
        touch?.hiddenByGamepad = pad && settings.hideWithGamepad
    }

    override fun onInputDeviceAdded(deviceId: Int) = updateGamepad()
    override fun onInputDeviceRemoved(deviceId: Int) = updateGamepad()
    override fun onInputDeviceChanged(deviceId: Int) = updateGamepad()

    companion object {
        const val EXTRA_ARGS = "snx.args"
    }
}
