package com.webheadnx.game

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.io.RandomAccessFile
import java.util.concurrent.atomic.AtomicBoolean

// Лаунчер: папка игры (импорт или путь), графика (пресеты по силе телефона), управление (редактор раскладки),
// дополнительно (кэш, переменные SNX_*, журнал). «Играть» запускает GameActivity в процессе :game.
class LauncherActivity : Activity() {
    private lateinit var s: Settings
    private lateinit var content: LinearLayout
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        s = Settings(this)
        // «Указать папку…» (чтение игры по пути, «Доступ ко всем файлам») убрано: Play Protect предупреждал об
        // опасном разрешении. Папка вне каталога приложения без разрешения не читается — снова папка по умолчанию
        val def = Settings.defaultGameDir(this)
        if (s.gameDir != def.absolutePath && GameFolder.check(s.gameDir) != null) s.gameDir = def.absolutePath
        build()
        // при первом запуске и после обновления — кто сделал порт и кому спасибо
        if (s.creditsShownVersion != BuildConfig.VERSION_CODE) {
            s.creditsShownVersion = BuildConfig.VERSION_CODE
            showCredits()
        }
        Updater.checkAndAsk(this, s, auto = true)  // новый выпуск на GitHub — «Скачать» / «Позже»
    }

    // окно «О порте»: CreditsDialog (автор, Telegram-канал, благодарность репозиторию webheadNX)
    private fun showCredits() = CreditsDialog.show(this)

    override fun onResume() {
        super.onResume()
        if (stale) {  // игра (процесс :game) или редактор могли поменять настройки: перечитать и перерисовать
            stale = false
            s = Settings(this)
            build()
        }
        refreshStatus()
        Updater.resumePending(this)  // вернулись из настроек «Установка неизвестных приложений»
        if (fromGame) { fromGame = false; explainFailedStart() }
    }

    private var fromGame = false

    // Игра не стартовала (host/boot/main.c: «[boot] process init failed») — вместо молчаливого возврата в лаунчер
    // объяснить причину по строкам загрузчика из snx.log
    private fun explainFailedStart() {
        val log = try { Settings.logFile(this).readText().takeLast(64 * 1024) } catch (e: Exception) { return }
        if (!log.contains("[boot] process init failed")) return
        val loader = log.lines().filter { it.startsWith("[loader]") }.takeLast(6)
        val missing = GameFolder.missingModules(s.gameDir, Settings.moduleDir(this))
        val msg = when {
            missing.isNotEmpty() -> GameFolder.missingText(missing)
            loader.any { it.contains("differs from the one used") } -> L.t(
                "Файлы игры не совпадают с теми, из которых собран этот APK (другая версия или копия игры, " +
                    "либо файл повреждён). Импортируйте ту же копию игры.",
                "The game files differ from the ones this APK was built from (another version or copy, or a damaged " +
                    "file). Import the same game copy.")
            else -> L.t("Игра не запустилась.", "The game failed to start.")
        } + (if (loader.isNotEmpty()) "\n\n" + loader.joinToString("\n") else "")
        AlertDialog.Builder(this).setTitle(L.t("Игра не запустилась", "The game failed to start")).setMessage(msg)
            .setPositiveButton(L.t("Импорт из папки…", "Import from folder…")) { _, _ -> pickImport() }
            .setNegativeButton("OK", null).show()
    }

    private var stale = false

    private fun build() {
        val scroll = ScrollView(this).apply { setBackgroundColor(Ui.BG) }
        content = Ui.vbox(this, 18)
        scroll.addView(content)
        setContentView(scroll)

        // заголовок и «Играть»
        val head = Ui.hbox(this).apply { setPadding(0, 0, 0, Ui.dp(this@LauncherActivity, 12f)) }
        val titles = Ui.vbox(this)
        titles.addView(Ui.title(this, getString(R.string.app_name), 28f))
        variantLabel = Ui.label(this, "", dim = true, size = 13f)
        titles.addView(variantLabel)
        status = Ui.label(this, "", size = 14f).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) }
        titles.addView(status)
        head.addView(titles, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(Ui.button(this, L.t("О порте", "About")) { showCredits() },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { rightMargin = Ui.dp(this@LauncherActivity, 8f) })
        head.addView(Ui.button(this, L.t("▶  Играть", "▶  Play"), primary = true) { play() })
        content.addView(head)

        val cols = Ui.hbox(this).apply { gravity = Gravity.TOP }
        val left = Ui.vbox(this)
        val right = Ui.vbox(this)
        val gap = Ui.dp(this, 12f)
        cols.addView(left, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = gap })
        cols.addView(right, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        content.addView(cols)

        left.addView(gameCard())
        left.addView(extrasCard())
        left.addView(controlsCard())
        right.addView(graphicsCard())
        right.addView(advancedCard())
    }

    // ---------------------------------------------------------------- папка игры
    private fun gameCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, L.t("Файлы игры", "Game files")))
        val path = Ui.label(this, s.gameDir, dim = true, size = 12f)
        c.addView(path)
        c.addView(Ui.label(this, L.t("Нужна папка установленной игры с ПК (System, Maps, Textures, Sounds…). Проще всего — " +
            "скопировать её по USB в папку по умолчанию:", "You need the folder of the game installed on a PC (System, Maps, " +
            "Textures, Sounds…). The easiest way is to copy it over USB to the default folder:") +
            "\n${Settings.defaultGameDir(this).absolutePath}",
            dim = true, size = 12f).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, Ui.dp(this@LauncherActivity, 8f)) })
        val row1 = Ui.hbox(this)
        row1.addView(Ui.button(this, L.t("Импорт из папки…", "Import from folder…")) { pickImport() }, wrapGap())
        c.addView(row1)
        val row2 = Ui.hbox(this).apply { setPadding(0, Ui.dp(this@LauncherActivity, 8f), 0, 0) }
        row2.addView(Ui.button(this, L.t("По умолчанию", "Default")) {
            s.gameDir = Settings.defaultGameDir(this).absolutePath
            Settings.defaultGameDir(this).mkdirs()
            path.text = s.gameDir
            refreshStatus()
        }, wrapGap())
        c.addView(row2)
        c.addView(Ui.label(this, L.t("Один APK поддерживает английскую и русскую игру: язык определяется по " +
            "System/*.int, сохранения — раздельные. Язык лаунчера — по языку телефона.",
            "One APK supports both English and Russian: the game language comes from System/*.int, saves are " +
            "separate. The launcher follows your device language."), dim = true, size = 12f))
        pathLabel = path
        return c
    }

    private var pathLabel: TextView? = null
    private var variantLabel: TextView? = null

    // «webheadNX 0.1.0-N · игра: русская (System/*.int: ru)» — язык определяется из папки игры
    private fun variantText(): String {
        val r = GameVariant.detect(s.gameDir)
        val lang = r.variant?.let { GameVariant.label(it) } ?: L.t("не определен", "unknown")
        return "webheadNX ${BuildConfig.VERSION_NAME} · " + L.t("игра: ", "game: ") + lang + " (${r.how})"
    }

    private fun wrapGap() = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        .apply { rightMargin = Ui.dp(this@LauncherActivity, 8f) }

    private fun refreshStatus() {
        if (!::status.isInitialized) return
        val mods = Settings.moduleDir(this)
        val missing = GameFolder.missingModules(s.gameDir, mods)
        val err = GameFolder.check(s.gameDir)
            ?: if (missing.isNotEmpty()) L.t("Пропали System/${missing.joinToString(", ")} — «Импорт из папки…»",
                "Missing System/${missing.joinToString(", ")} — “Import from folder…”") else null
        val backup = GameFolder.fromBackup(s.gameDir, mods)
        val movieWarn = GameFolder.movieProblem(s.gameDir)
        status.text = (err?.let { "⚠ $it" } ?: (L.t("✓ Игра найдена", "✓ Game found") +
            if (backup.isNotEmpty()) L.t(" (${backup.joinToString(", ")} — из копии приложения)",
                " (${backup.joinToString(", ")} from the app's copy)") else "")) +
            (if (movieWarn != null) "\n⚠ $movieWarn" else "")
        status.setTextColor(if (err == null && movieWarn == null) Ui.ACCENT else 0xFFFFB74D.toInt())
        pathLabel?.text = s.gameDir
        variantLabel?.text = variantText()
    }

    private fun pickImport() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_IMPORT)
    }

    @Deprecated("Activity API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data ?: return
        if (resultCode != RESULT_OK) return
        if (requestCode == REQ_IMPORT) runImport(uri)
        if (requestCode == REQ_SAVES) runSaveImport(uri)
    }

    private fun runImport(tree: Uri) {
        val dst = Settings.defaultGameDir(this)
        val box = Ui.vbox(this, 20)
        val what = Ui.label(this, L.t("Подготовка…", "Preparing…"))
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { isIndeterminate = true }
        box.addView(what)
        box.addView(bar)
        val cancelled = AtomicBoolean(false)
        val dlg = AlertDialog.Builder(this).setTitle(L.t("Импорт игры", "Importing the game")).setView(box).setCancelable(false)
            .setNegativeButton(L.t("Отмена", "Cancel")) { _, _ -> cancelled.set(true) }.show()
        Thread {
            var error: String? = null
            try {
                GameFolder.import(contentResolver, tree, dst, { cancelled.get() }) { p ->
                    runOnUiThread { what.text = L.t("${p.files} файлов, ${p.bytes / (1 shl 20)} МБ\n${p.current}", "${p.files} files, ${p.bytes / (1 shl 20)} MB\n${p.current}") }
                }
            } catch (e: Exception) {
                error = e.message ?: e.toString()
            }
            runOnUiThread {
                dlg.dismiss()
                if (!cancelled.get()) {
                    s.gameDir = dst.absolutePath
                    refreshStatus()
                    toastDialog(error?.let { L.t("Ошибка импорта: $it", "Import failed: $it") } ?: (GameFolder.check(dst.absolutePath)
                        ?.let { L.t("Скопировано, но: $it", "Copied, but: $it") } ?: L.t("Готово: игра скопирована.", "Done: the game has been copied.")))
                }
            }
        }.start()
    }

    // ---------------------------------------------------------------- уровни, сохранения, читы
    private fun extrasCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, L.t("Уровни, сохранения, читы", "Levels, saves, cheats")))
        val row = Ui.hbox(this)
        row.addView(Ui.button(this, L.t("Выбор уровня…", "Level select…")) {
            Levels.show(this, s.gameDir) { map -> launch(map) }
        }, wrapGap())
        row.addView(Ui.button(this, L.t("Импорт сохранений…", "Import saves…")) {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_SAVES)
        }, wrapGap())
        c.addView(HorizontalScrollView(this).apply { isHorizontalScrollBarEnabled = false; addView(row) })
        c.addView(Ui.label(this, L.t("Сохранения — папка Save с ПК (или папка, в которой она лежит). Прежние сохранения порта " +
            "не пропадут: они переименуются в Save.bak-<дата>.", "Saves: the Save folder from a PC (or the folder containing it). " +
            "The current saves are kept as Save.bak-<date>."), dim = true, size = 12f)
            .apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) })
        c.addView(Ui.label(this, L.t("Читы Spider-Man 2 включаются в «Дополнительно» и открываются шестерёнкой в игре.",
            "Spider-Man 2 cheats are enabled in Advanced and opened by the gear button in game."), dim = true, size = 12f))
        return c
    }

    private fun runSaveImport(tree: Uri) {
        val dlg = AlertDialog.Builder(this).setMessage(L.t("Копирую сохранения…", "Copying saves…")).setCancelable(false).show()
        Thread {
            val res = try { Result.success(Saves.import(this, s, tree)) } catch (e: Exception) { Result.failure(e) }
            runOnUiThread {
                dlg.dismiss()
                val r = res.getOrNull()
                toastDialog(if (r == null) L.t("Ошибка: ${res.exceptionOrNull()?.message}", "Error: ${res.exceptionOrNull()?.message}")
                    else L.t("Готово: ${r.files} файлов → ${r.target.path}", "Done: ${r.files} files → ${r.target.path}") +
                        (r.backup?.let { L.t("\nПрежние сохранения: ${it.name}", "\nPrevious saves: ${it.name}") } ?: ""))
            }
        }.start()
    }

    // ---------------------------------------------------------------- графика
    private fun graphicsCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, L.t("Графика и скорость", "Graphics and speed")))
        val tier = Device.tier(this)
        val ram = "%.1f".format(Device.ramMb(this) / 1024.0)
        c.addView(Ui.label(this, L.t("Телефон: $ram ГБ ОЗУ, ЦП до ${Device.maxFreqMhz()} МГц → «${tier.name}»", "Phone: $ram GB RAM, CPU up to ${Device.maxFreqMhz()} MHz → “${tier.name}”"),
            dim = true, size = 12f))
        val holder = Ui.vbox(this)
        fun fill() {
            holder.removeAllViews()
            val scales = listOf(0.5f, 0.6f, 0.75f, 0.85f, 1.0f)
            holder.addView(Ui.choiceRow(this, L.t("Разрешение (доля экрана)", "Resolution (share of screen)"), scales.map { "${(it * 100).toInt()} %" },
                scales.indexOfFirst { it >= s.renderScale - 0.01f }.coerceAtLeast(0)) { s.renderScale = scales[it] })
            val fps = listOf(30, 45, 60, 0)
            holder.addView(Ui.choiceRow(this, L.t("Ограничение FPS", "FPS limit"), listOf("30", "45", "60", L.t("Нет", "Off")),
                fps.indexOf(s.fpsCap).let { if (it < 0) 3 else it }) { s.fpsCap = fps[it] })
            holder.addView(Ui.label(this, L.t("«Нет» — без ограничения: экран на самой высокой частоте, без vsync (сильнее греется и садит батарею)",
                "“Off” — uncapped: the screen at its highest refresh rate, no vsync (more heat and battery drain)"), dim = true, size = 12f))
            holder.addView(Ui.choiceRow(this, L.t("Формат кадра", "Aspect ratio"),
                listOf(L.t("Весь экран", "Full screen"), "16:9", "4:3"), s.aspect.coerceIn(0, 2)) { s.aspect = it })
            holder.addView(Ui.label(this, L.t(
                "Весь экран — на широких телефонах (20:9, 21:9) мир шире по бокам, но меню и интерфейс игры растянуты. 16:9 и 4:3 — поля по бокам, интерфейс ближе к оригиналу (4:3 — как на ПК).",
                "Full screen — on wide phones (20:9, 21:9) you see more of the world, but the game menus and HUD are stretched. 16:9 and 4:3 add side bars, the HUD looks closer to the original (4:3 — as on PC)."),
                dim = true, size = 12f))
            holder.addView(Ui.switchRow(this, L.t("16-битные текстуры", "16-bit textures"), L.t("Меньше памяти и быстрее на слабых GPU, чуть хуже цвета", "Less memory and faster on weak GPUs, slightly worse colors"),
                s.tex16) { s.tex16 = it })
            holder.addView(Ui.switchRow(this, L.t("Стабильная производительность", "Sustained performance"),
                L.t("Частоты ниже пика, зато без перегрева и падения FPS через 10 минут (если телефон умеет)", "Lower peak clocks, but no overheating and FPS drops after 10 minutes (if supported)"), s.sustained) { s.sustained = it })
            holder.addView(Ui.switchRow(this, L.t("Счётчик FPS", "FPS counter"), null, s.showFps) { s.showFps = it })
        }
        c.addView(Ui.choiceRow(this, L.t("Пресет", "Preset"), listOf(Device.LOW.name, Device.MID.name, Device.HIGH.name),
            listOf(Device.LOW, Device.MID, Device.HIGH).indexOfFirst { it.scale == s.renderScale && it.fps == s.fpsCap }
                .let { if (it < 0) 1 else it }) {
            s.applyTier(listOf(Device.LOW, Device.MID, Device.HIGH)[it])
            fill()
        })
        fill()
        c.addView(holder)
        return c
    }

    // ---------------------------------------------------------------- управление
    private fun controlsCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, L.t("Управление", "Controls")))
        c.addView(Ui.button(this, L.t("✎  Редактор раскладки", "✎  Layout editor")) {
            startActivity(Intent(this, LayoutEditorActivity::class.java))
        })
        c.addView(Ui.label(this, L.t("В игре — шестерёнка в левом верхнем углу. Палец на свободном месте экрана — камера.", "In game: the gear in the top-left corner. A finger on a free area of the screen moves the camera."),
            dim = true, size = 12f).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) })
        c.addView(Ui.switchRow(this, L.t("Экранное управление", "Touch controls"), null, s.touchEnabled) { s.touchEnabled = it })
        c.addView(Ui.switchRow(this, L.t("Прятать при геймпаде", "Hide with a gamepad"), L.t("Подключён геймпад — кнопки на экране не нужны", "A gamepad is connected — no need for on-screen buttons"),
            s.hideWithGamepad) { s.hideWithGamepad = it })
        c.addView(Ui.switchRow(this, L.t("Вибрация кнопок", "Button vibration"), null, s.haptics) { s.haptics = it })
        c.addView(Ui.seekRow(this, L.t("Чувствительность камеры", "Camera sensitivity"), 38, ((s.lookSensitivity - 0.1f) * 10f).toInt(),
            { "%.1f".format(0.1f + it / 10f) }) { s.lookSensitivity = 0.1f + it / 10f })
        c.addView(Ui.choiceRow(this, L.t("Камера правым стиком геймпада", "Gamepad right stick camera"),
            listOf(L.t("Как палец", "Like touch"), L.t("Ось джойстика", "Joystick axis")), s.padCamera.coerceIn(0, 1)) { s.padCamera = it })
        c.addView(Ui.label(this, L.t("«Как палец» — камера не пропадает (было на Retroid Pocket, GameSir); скорость — ползунок выше.",
            "“Like touch” keeps the camera from dropping out (seen on Retroid Pocket, GameSir); speed is the slider above."), dim = true, size = 12f))
        c.addView(Ui.seekRow(this, L.t("Прозрачность кнопок", "Button opacity"), 18, ((s.opacity - 0.1f) * 20f).toInt(),
            { "${((0.1f + it / 20f) * 100).toInt()} %" }) { s.opacity = 0.1f + it / 20f })
        return c
    }

    // ---------------------------------------------------------------- дополнительно
    private fun advancedCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, L.t("Дополнительно", "Advanced")))
        val caches = listOf(64, 96, 128, 256, 512, 1024)
        c.addView(Ui.choiceRow(this, L.t("Кэш файлов игры в памяти, МБ", "Game file cache in memory, MB"), caches.map { it.toString() },
            caches.indexOf(s.fileCacheMb).let { if (it < 0) 3 else it }) { s.fileCacheMb = caches[it] })
        val row = Ui.hbox(this).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) }
        row.addView(Ui.button(this, L.t("Журнал", "Log")) { showLog() }, wrapGap())
        row.addView(Ui.button(this, L.t("Переменные SNX_*", "SNX_* variables")) { editEnv() }, wrapGap())
        c.addView(row)
        c.addView(Ui.switchRow(this, L.t("Читы в игре", "In-game cheats"),
            L.t("Меню шестерёнки → «Читы»: noclip, полёт, бессмертие и другие команды", "Gear menu → Cheats: noclip, fly, god mode and more"),
            s.cheats) { s.cheats = it })
        c.addView(Ui.switchRow(this, L.t("Проверять обновления", "Check for updates"),
            L.t("При запуске, раз в 6 часов: новая версия на GitHub — предложит скачать", "On start, every 6 hours: offers a new version from GitHub"),
            s.checkUpdates) { s.checkUpdates = it })
        c.addView(Ui.button(this, L.t("Проверить обновления", "Check for updates")) { Updater.checkAndAsk(this, s, auto = false) }
            .apply { layoutParams = wrapGap().apply { bottomMargin = Ui.dp(this@LauncherActivity, 4f) } })
        c.addView(Ui.button(this, L.t("Очистить кэш шейдеров", "Clear shader cache")) {
            File(cacheDir, "shaders").deleteRecursively()
            File(Settings.userDir(this), "System/SnxPrograms.bin").delete()  // список программ для фоновой сборки
            toastDialog(L.t("Кэш шейдеров очищен: при следующем запуске они соберутся заново.", "Shader cache cleared: shaders will be rebuilt on the next start."))
        }.apply { layoutParams = wrapGap().apply { topMargin = Ui.dp(this@LauncherActivity, 8f) } })
        return c
    }

    private fun editEnv() {
        val e = EditText(this).apply {
            setText(s.extraEnv)
            hint = "SNX_NOVSYNC=1"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minLines = 4
            setTextColor(Ui.TEXT)
        }
        AlertDialog.Builder(this).setTitle(L.t("Переменные окружения (ИМЯ=ЗНАЧЕНИЕ)", "Environment variables (NAME=VALUE)")).setView(e)
            .setPositiveButton(L.t("Сохранить", "Save")) { _, _ -> s.extraEnv = e.text.toString() }
            .setNegativeButton(L.t("Отмена", "Cancel"), null).show()
    }

    private fun showLog() {
        val f = Settings.logFile(this)
        val text = if (!f.exists()) L.t("Журнала пока нет — запусти игру.", "No log yet — start the game.") else try {
            RandomAccessFile(f, "r").use { r ->
                val n = minOf(r.length(), 64L * 1024)
                r.seek(r.length() - n)
                val b = ByteArray(n.toInt())
                r.readFully(b)
                String(b)
            }
        } catch (e: Exception) { e.toString() }
        val tv = TextView(this).apply {
            this.text = text
            textSize = 11f
            setTextIsSelectable(true)
            typeface = android.graphics.Typeface.MONOSPACE
            val p = Ui.dp(this@LauncherActivity, 12f)
            setPadding(p, p, p, p)
        }
        val sv = ScrollView(this).apply { addView(tv); post { fullScroll(ScrollView.FOCUS_DOWN) } }
        AlertDialog.Builder(this).setTitle(f.absolutePath).setView(sv)
            .setPositiveButton(L.t("Скопировать", "Copy")) { _, _ ->
                (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("snx.log", text))
            }.setNeutralButton(L.t("Поделиться", "Share")) { _, _ -> shareLogs() }
            .setNegativeButton(L.t("Закрыть", "Close"), null).show()
    }

    // Отчёт для разработчика: журнал последнего запуска и прошлого (начало и конец каждого) — текстом в любое
    // приложение и файлом в «Загрузки» (папка Android/data файловым менеджерам на Android 11+ не видна)
    private fun shareLogs() {
        fun part(f: File): String {
            if (!f.exists()) return "(нет файла)\n"
            return try {
                RandomAccessFile(f, "r").use { r ->
                    val len = r.length()
                    fun read(from: Long, n: Long): String {
                        r.seek(from)
                        val b = ByteArray(n.toInt())
                        r.readFully(b)
                        return String(b)
                    }
                    val head = 24L * 1024
                    val tail = 40L * 1024
                    if (len <= head + tail) read(0, len)
                    else read(0, head) + "\n…\n" + read(len - tail, tail)
                }
            } catch (e: Exception) {
                e.toString()
            }
        }
        val log = Settings.logFile(this)
        val report = "webheadNX ${BuildConfig.VERSION_NAME} (${Settings.language(this)}, ${GameVariant.detect(s.gameDir).how}), ${Build.MANUFACTURER} ${Build.MODEL}, " +
            "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n\n" +
            "===== snx.log (последний запуск) =====\n" + part(log) +
            "\n\n===== snx.log.old (запуск перед ним) =====\n" + part(File(log.path + ".old"))
        var saved = ""
        if (Build.VERSION.SDK_INT >= 29) {
            try {
                val v = android.content.ContentValues().apply {
                    put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, "webheadNX-log-${System.currentTimeMillis() / 1000}.txt")
                    put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "text/plain")
                    put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                }
                contentResolver.insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, v)?.let { uri ->
                    contentResolver.openOutputStream(uri)?.use { it.write(report.toByteArray()) }
                    saved = L.t("Файл сохранён в «Загрузки».", "The file is saved to Downloads.")
                }
            } catch (e: Exception) {
            }
        }
        val send = Intent(Intent.ACTION_SEND).setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, "webheadNX log")
            .putExtra(Intent.EXTRA_TEXT, report)
        try {
            startActivity(Intent.createChooser(send, L.t("Отправить журнал", "Send the log")))
        } catch (e: Exception) {
            if (saved.isNotEmpty()) toastDialog(saved)
        }
        if (saved.isNotEmpty()) android.widget.Toast.makeText(this, saved, android.widget.Toast.LENGTH_LONG).show()
    }

    // ---------------------------------------------------------------- запуск
    private fun play() {
        val err = GameFolder.check(s.gameDir)
        if (err != null) {
            AlertDialog.Builder(this).setTitle(L.t("Игра не найдена", "Game not found")).setMessage(err)
                .setPositiveButton(L.t("Всё равно запустить", "Start anyway")) { _, _ -> launch() }
                .setNegativeButton(L.t("Отмена", "Cancel"), null).show()
            return
        }
        val missing = GameFolder.missingModules(s.gameDir, Settings.moduleDir(this))
        if (missing.isNotEmpty()) {
            AlertDialog.Builder(this).setTitle(L.t("Не хватает файлов игры", "Game files are missing"))
                .setMessage(GameFolder.missingText(missing))
                .setPositiveButton(L.t("Импорт из папки…", "Import from folder…")) { _, _ -> pickImport() }
                .setNeutralButton(L.t("Всё равно запустить", "Start anyway")) { _, _ -> launch() }
                .setNegativeButton(L.t("Отмена", "Cancel"), null).show()
            return
        }
        launch()
    }

    private fun launch(map: String? = null) {
        stale = true
        fromGame = true
        // Лаунчер виден — значит, игры поверх него нет (она в той же задаче). Живой процесс :game тогда — остаток
        // прошлого запуска (завис на выходе): новая игра попала бы в него и не отвечала бы на касания. Завершить
        // и дождаться, пока система его отпустит (до 2 с), затем — запуск с чистого листа.
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        fun gamePid(): Int? = am.runningAppProcesses
            ?.firstOrNull { it.processName == "$packageName:game" && it.pid != Process.myPid() }?.pid
        val start = { startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.EXTRA_ARGS, s.gameArgs(map))) }
        val pid = gamePid() ?: return start()
        Process.killProcess(pid)
        val h = Handler(Looper.getMainLooper())
        val t0 = SystemClock.uptimeMillis()
        h.post(object : Runnable {
            override fun run() {
                if (gamePid() == null || SystemClock.uptimeMillis() - t0 > 2000) {
                    h.postDelayed({ start() }, 150)
                } else h.postDelayed(this, 50)
            }
        })
    }

    private fun toastDialog(msg: String) {
        AlertDialog.Builder(this).setMessage(msg).setPositiveButton("OK", null).show()
    }

    companion object {
        private const val REQ_IMPORT = 1
        private const val REQ_SAVES = 2
    }
}
