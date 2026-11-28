package com.webheadnx.game

import android.app.ActivityManager
import android.content.Context
import android.content.SharedPreferences
import java.io.File

// Настройки лаунчера (SharedPreferences). Пишутся сразу (commit): процесс игры (:game) стартует следом и читает
// их с диска. Раскладка экранного управления — отдельным файлом (TouchLayout): её правят оба процесса.
class Settings(private val ctx: Context) {
    @Suppress("DEPRECATION")
    private val p: SharedPreferences = ctx.getSharedPreferences("snx", Context.MODE_MULTI_PROCESS)  // лаунчер и :game

    init {
        // прежние сборки: «без ограничения» по умолчанию у среднего и мощного пресетов — один раз на 60
        if (!p.getBoolean("fps60", false)) {
            val e = p.edit().putBoolean("fps60", true)
            if (p.getInt("fpsCap", -1) == 0) e.putInt("fpsCap", 60)
            e.commit()
        }
    }

    var gameDir: String
        get() = p.getString("gameDir", null) ?: defaultGameDir(ctx).absolutePath
        set(v) { p.edit().putString("gameDir", v).commit() }

    // графика
    var renderScale: Float
        get() = p.getFloat("renderScale", Device.tier(ctx).scale)
        set(v) { p.edit().putFloat("renderScale", v).commit() }
    var fpsCap: Int  // 0 — без ограничения (по частоте экрана, vsync)
        get() = p.getInt("fpsCap", Device.tier(ctx).fps)
        set(v) { p.edit().putInt("fpsCap", v).commit() }
    var tex16: Boolean  // DXT -> 16 бит при распаковке на ЦП: вдвое меньше видеопамяти
        get() = p.getBoolean("tex16", Device.tier(ctx).tex16)
        set(v) { p.edit().putBoolean("tex16", v).commit() }
    var aspect: Int  // формат кадра: 0 — во весь экран (Hor+), 1 — не шире 16:9, 2 — 4:3 (интерфейс без растяжения)
        get() = p.getInt("aspect", 0)
        set(v) { p.edit().putInt("aspect", v).commit() }
    var showFps: Boolean
        get() = p.getBoolean("showFps", false)
        set(v) { p.edit().putBoolean("showFps", v).commit() }

    var sustained: Boolean  // стабильная производительность: ниже пик, зато без троттлинга (если телефон умеет)
        get() = p.getBoolean("sustained", false)
        set(v) { p.edit().putBoolean("sustained", v).commit() }
    var fileCacheMb: Int
        get() = p.getInt("fileCacheMb", Device.tier(ctx).fileCacheMb)
        set(v) { p.edit().putInt("fileCacheMb", v).commit() }
    var extraEnv: String  // строки ИМЯ=ЗНАЧЕНИЕ (для опытных: переменные SNX_* из docs/CONSOLE.md)
        get() = p.getString("extraEnv", "") ?: ""
        set(v) { p.edit().putString("extraEnv", v).commit() }

    // управление
    var touchEnabled: Boolean
        get() = p.getBoolean("touchEnabled", true)
        set(v) { p.edit().putBoolean("touchEnabled", v).commit() }
    var hideWithGamepad: Boolean
        get() = p.getBoolean("hideWithGamepad", true)
        set(v) { p.edit().putBoolean("hideWithGamepad", v).commit() }
    var opacity: Float
        get() = p.getFloat("opacity", 0.6f)
        set(v) { p.edit().putFloat("opacity", v).commit() }
    // Чувствительность камеры = «чувствительность мыши»: сколько градусов поворота даёт жест по всей высоте
    // экрана (1.6 → 144°, то есть сдвиг пальца на десятую часть экрана ≈ 14°). Ключ lookSens3: камера пальцем
    // теперь идёт отсчётами мыши, и подобранные в старых сборках значения (там ползунком лечили «не слушается»)
    // к ней не относятся — чистый старт с живого, но не дёрганого значения.
    var lookSensitivity: Float
        get() = p.getFloat("lookSens3", 1.6f)
        set(v) { p.edit().putFloat("lookSens3", v).commit() }
    // Камера: из настроек остались ровно две вещи — чувствительность (выше: «чувствительность мыши») и зеркало
    // осей. Мёртвой зоны и «скорости поворота» больше нет: это параметры оси правого стика геймпада, а камера
    // пальцем идёт отсчётами мыши, как мышь в ПК-версии.
    var camInvX: Boolean
        get() = p.getBoolean("camInvX", false)
        set(v) { p.edit().putBoolean("camInvX", v).commit() }
    var camInvY: Boolean
        get() = p.getBoolean("camInvY", false)
        set(v) { p.edit().putBoolean("camInvY", v).commit() }
    // «Держать угол»: игра любит сама доворачивать камеру за спину герою — из-за этого она «отщёлкивалась назад».
    // Включено по умолчанию: порт мягко возвращает камеру туда, куда её поставил игрок.
    var camHold: Boolean
        get() = p.getBoolean("camHold", true)
        set(v) { p.edit().putBoolean("camHold", v).commit() }
    var potionSlot: Int  // слот зелья (1–9) у кнопки «Зелья»: стрелки рядом с ней, короткое нажатие — выпить
        get() = p.getInt("potionSlot", 1).coerceIn(1, 9)
        set(v) { p.edit().putInt("potionSlot", v.coerceIn(1, 9)).apply() }
    // Язык интерфейса: 0 — по языку телефона, 1 — русский, 2 — English (лаунчер и игра)
    var langForce: Int
        get() = p.getInt("langForce", 0).coerceIn(0, 2)
        set(v) { p.edit().putInt("langForce", v.coerceIn(0, 2)).commit() }
    var haptics: Boolean
        get() = p.getBoolean("haptics", true)
        set(v) { p.edit().putBoolean("haptics", v).commit() }
    var cheats: Boolean  // читы: привязки команд в User.ini (host/boot/config.c) и меню «Читы» в игре (Cheats)
        get() = p.getBoolean("cheats", false)
        set(v) { p.edit().putBoolean("cheats", v).commit() }

    // обновления (Updater): проверять при запуске лаунчера; когда проверяли; «Позже» — не спрашивать до
    var checkUpdates: Boolean
        get() = p.getBoolean("checkUpdates", true)
        set(v) { p.edit().putBoolean("checkUpdates", v).commit() }
    var updateCheckedAt: Long
        get() = p.getLong("updateCheckedAt", 0L)
        set(v) { p.edit().putLong("updateCheckedAt", v).commit() }
    var updateSnoozeUntil: Long
        get() = p.getLong("updateSnoozeUntil", 0L)
        set(v) { p.edit().putLong("updateSnoozeUntil", v).commit() }

    var creditsShownVersion: Int  // versionCode, для которого уже показано окно «О порте»
        get() = p.getInt("creditsShown", 0)
        set(v) { p.edit().putInt("creditsShown", v).commit() }

    fun applyTier(t: Device.Tier) {
        renderScale = t.scale
        fpsCap = t.fps
        tex16 = t.tex16
        fileCacheMb = t.fileCacheMb
    }

    // аргументы libmain.so (host/boot/main.c)
    // map — сразу на этот уровень («Выбор уровня»: имя файла из Maps, например 1_Shreks_Swamp.unr)
    fun gameArgs(map: String? = null): Array<String> {
        val user = userDir(ctx)
        user.mkdirs()
        val a = arrayListOf("--game", gameDir, "--user", user.absolutePath)
        fun env(k: String, v: String) { a += "--env"; a += "$k=$v" }
        env("SNX_RENDER_SCALE", "%.3f".format(java.util.Locale.ROOT, renderScale))
        if (fpsCap > 0) env("SNX_FPS_CAP", fpsCap.toString())
        else env("SNX_NOVSYNC", "1")  // «Нет»: без vsync — кадров больше частоты экрана (platform/window.c)
        when (aspect) { 1 -> env("SNX_ASPECT", "16:9"); 2 -> env("SNX_ASPECT", "4:3") }
        if (tex16) env("SNX_TEX16", "1")
        env("SNX_SHOW_FPS", if (showFps) "1" else "0")
        env("SNX_FILE_CACHE_MB", fileCacheMb.toString())
        env("SNX_LOOK_SENS", "%.2f".format(java.util.Locale.ROOT, lookSensitivity))  // и камера правым стиком
        env("SNX_CAM_HOLD", if (camHold) "1" else "0")
        env("SNX_CAM_INV_X", if (camInvX) "1" else "0")
        env("SNX_CAM_INV_Y", if (camInvY) "1" else "0")
        if (cheats) env("SNX_CHEATS", "1")
        // копия Webhead.exe и .dll во внутренней памяти (runtime/loader.c): их не удалит очистка или антивирус
        env("SNX_MODULE_DIR", moduleDir(ctx).apply { mkdirs() }.absolutePath)
        val tahoma = File(gameDir, "System/tahoma.ttf")
        if (tahoma.exists()) env("SNX_GDI_FONT", tahoma.absolutePath)
        for (line in extraEnv.lines()) {
            val s = line.trim()
            if (s.isNotEmpty() && !s.startsWith("#") && s.contains('=')) { a += "--env"; a += s }
        }
        if (!map.isNullOrEmpty()) { a += "--"; a += map }  // после «--» — командная строка игры (host/boot/main.c)
        return a.toTypedArray()
    }

    companion object {
        // язык игры в папке — по System/*.int (один APK на eng и rus)
        fun language(ctx: Context): String = GameVariant.effective(Settings(ctx))
        fun defaultGameDir(ctx: Context): File = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "game")
        // настройки и сохранения: у русской локализации свои ini и сейвы — в user-rus
        fun userDir(ctx: Context): File = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir,
            if (language(ctx) == GameVariant.RUS) "user-rus" else "user")
        fun logFile(ctx: Context): File = File(userDir(ctx), "snx.log")
        // внутренняя память приложения (не Android/data): копии модулей игры, у локализаций — свои
        fun moduleDir(ctx: Context): File = File(ctx.filesDir, "modules/" + language(ctx))
    }
}

// Оценка телефона по памяти и ЦП: стартовые настройки, чтобы игра шла и на слабых
object Device {
    data class Tier(val name: String, val scale: Float, val fps: Int, val tex16: Boolean, val fileCacheMb: Int)

    val LOW = Tier(L.t("Слабый", "Low"), 0.5f, 30, true, 96)
    // 60 FPS по умолчанию: экран переключается на 60 Гц, кадры отмеряет vsync — без рывков, которые были
    // без ограничения на экранах 90/120 Гц (игра не держит 120 — кадры по 8/17/25 мс вперемешку)
    val MID = Tier(L.t("Средний", "Medium"), 0.75f, 60, false, 256)
    val HIGH = Tier(L.t("Мощный", "High"), 1.0f, 60, false, 512)

    fun ramMb(ctx: Context): Long {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo()
        am.getMemoryInfo(mi)
        return mi.totalMem / (1024 * 1024)
    }

    fun maxFreqMhz(): Int {
        var best = 0
        for (c in 0 until Runtime.getRuntime().availableProcessors()) {
            val f = File("/sys/devices/system/cpu/cpu$c/cpufreq/cpuinfo_max_freq")
            val v = try { f.readText().trim().toInt() / 1000 } catch (e: Exception) { 0 }
            if (v > best) best = v
        }
        return best
    }

    fun tier(ctx: Context): Tier {
        val ram = ramMb(ctx)
        val mhz = maxFreqMhz()
        return when {
            ram < 3500 || (mhz in 1..2000) -> LOW
            ram < 7000 || (mhz in 1..2600) -> MID
            else -> HIGH
        }
    }
}
