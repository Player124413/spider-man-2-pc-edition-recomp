package com.shreknx.game

import android.app.Activity
import android.app.AlertDialog
import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.provider.Settings as SysSettings
import android.widget.ProgressBar
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipFile

// Обновления из GitHub Releases (репозиторий порта): выпуск с APK того же вида, что установлен, и номером сборки
// больше своего — окно «Доступна версия…» с «Скачать» / «Позже». APK скачивается в кэш приложения, установку
// подтверждает пользователь (системный установщик; разрешение «Установка неизвестных приложений» — один раз).
// Имена APK — как в CI (.github/workflows/android.yml, шаг «Имя файла»): Webhead2-eng-0.1.0-<N>.apk — с
// игрой, webheadNX-nogame-<…>-0.1.0-<N>.apk — без; N — число коммитов (versionName), его и сравниваем.
object Updater {
    const val REPO = "Player124413/spider-man-2-pc-edition-recomp"
    private const val API = "https://api.github.com/repos/$REPO/releases?per_page=15"
    private const val AUTO_EVERY_MS = 6L * 3600 * 1000      // сама проверка при запуске — не чаще раза в 6 ч
    private const val SNOOZE_MS = 24L * 3600 * 1000         // «Позже» — на сутки

    class Release(val tag: String, val notes: String, val asset: String, val url: String, val size: Long, val build: Int)

    fun ownBuild(): Int = BuildConfig.VERSION_NAME.substringAfterLast('-').toIntOrNull() ?: 0

    // в этом APK — код игры (libmain.so с переведённым кодом — десятки МБ; без игры — около 4,5 МБ)
    fun hasGame(ctx: Context): Boolean = try {
        ZipFile(ctx.applicationInfo.sourceDir).use { z -> (z.getEntry("lib/arm64-v8a/libmain.so")?.size ?: 0L) > 12L * 1024 * 1024 }
    } catch (e: Exception) { true }

    // подходящие виды APK, по предпочтению: английский можно обновить и универсальным (тот же пакет)
    private fun variants(): List<String> = when (BuildConfig.VARIANT) {
        "eng" -> listOf("eng", "uni")
        "rus" -> listOf("rus")
        else -> listOf("uni")
    }

    // сеть — не из главного потока
    fun check(ctx: Context): Release? {
        val c = URL(API).openConnection() as HttpURLConnection
        c.connectTimeout = 10000
        c.readTimeout = 15000
        c.setRequestProperty("Accept", "application/vnd.github+json")
        c.setRequestProperty("User-Agent", "webheadNX-launcher")
        try {
            if (c.responseCode != 200) throw IllegalStateException("GitHub: HTTP ${c.responseCode}")
            val arr = JSONArray(c.inputStream.bufferedReader().readText())
            val prefix = if (hasGame(ctx)) "Webhead2-" else "webheadNX-nogame-"
            var best: Release? = null
            var bestRank = Int.MAX_VALUE
            for (i in 0 until arr.length()) {
                val r = arr.getJSONObject(i)
                if (r.optBoolean("draft") || r.optBoolean("prerelease")) continue
                val assets = r.optJSONArray("assets") ?: continue
                for (j in 0 until assets.length()) {
                    val a = assets.getJSONObject(j)
                    val name = a.optString("name")
                    for ((rank, v) in variants().withIndex()) {
                        val m = Regex("^" + Regex.escape(prefix + v + "-") + "[0-9.]+-(\\d+)\\.apk$").find(name) ?: continue
                        val build = m.groupValues[1].toInt()
                        val cur = best
                        if (cur == null || build > cur.build || (build == cur.build && rank < bestRank)) {
                            best = Release(r.optString("tag_name"), r.optString("body"), name, a.optString("browser_download_url"),
                                a.optLong("size"), build)
                            bestRank = rank
                        }
                    }
                }
            }
            return best?.takeIf { it.build > ownBuild() && it.url.startsWith("https://") }
        } finally {
            c.disconnect()
        }
    }

    fun dir(ctx: Context): File = File(ctx.externalCacheDir ?: ctx.cacheDir, "updates")

    // скачивание (переходы GitHub -> хранилище — сам HttpURLConnection, оба https); onProgress(байт, всего)
    fun download(ctx: Context, r: Release, cancel: () -> Boolean, onProgress: (Long, Long) -> Unit): File? {
        val d = dir(ctx)
        d.deleteRecursively()
        d.mkdirs()
        val out = File(d, r.asset.replace(Regex("[^A-Za-z0-9._-]"), "_"))
        val tmp = File(d, out.name + ".part")
        val c = URL(r.url).openConnection() as HttpURLConnection
        c.connectTimeout = 15000
        c.readTimeout = 30000
        c.setRequestProperty("User-Agent", "webheadNX-launcher")
        try {
            if (c.responseCode != 200) throw IllegalStateException("HTTP ${c.responseCode}")
            val total = c.contentLengthLong.takeIf { it > 0 } ?: r.size
            var got = 0L
            c.inputStream.use { i ->
                tmp.outputStream().use { o ->
                    val buf = ByteArray(1 shl 16)
                    while (true) {
                        if (cancel()) return null
                        val n = i.read(buf)
                        if (n < 0) break
                        o.write(buf, 0, n)
                        got += n
                        onProgress(got, total)
                    }
                }
            }
            if (total > 0 && got != total) throw IllegalStateException(L.t("загрузка оборвалась", "the download was cut off"))
        } finally {
            c.disconnect()
        }
        // тот же пакет и версия новее — иначе установщик всё равно откажет, но с непонятной ошибкой
        @Suppress("DEPRECATION")
        val info = ctx.packageManager.getPackageArchiveInfo(tmp.path, 0)
            ?: throw IllegalStateException(L.t("файл не похож на APK", "the file is not an APK"))
        if (info.packageName != ctx.packageName)
            throw IllegalStateException(L.t("в APK другой пакет (${info.packageName})", "the APK has another package (${info.packageName})"))
        if (!tmp.renameTo(out)) throw IllegalStateException(L.t("не записать файл", "cannot write the file"))
        return out
    }

    // системный установщик; без разрешения на установку — сначала в настройки (вернётся в onResume лаунчера)
    fun install(a: Activity, apk: File): Boolean {
        if (Build.VERSION.SDK_INT >= 26 && !a.packageManager.canRequestPackageInstalls()) {
            a.startActivity(Intent(SysSettings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${a.packageName}")))
            return false
        }
        val uri = Uri.parse("content://${a.packageName}.apk/${Uri.encode(apk.name)}")
        a.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri, ApkProvider.MIME)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK))
        return true
    }

    // ---------------------------------------------------------------- окна лаунчера
    var pending: File? = null  // скачано, ждёт разрешения на установку

    // auto: при запуске (не чаще раза в 6 ч, не после «Позже»; молча, если нового нет или сети нет)
    fun checkAndAsk(a: Activity, s: Settings, auto: Boolean) {
        val now = System.currentTimeMillis()
        if (auto && (!s.checkUpdates || now < s.updateSnoozeUntil || now - s.updateCheckedAt < AUTO_EVERY_MS)) return
        Thread {
            val res = try { Result.success(check(a)) } catch (e: Exception) { Result.failure(e) }
            a.runOnUiThread {
                if (a.isFinishing) return@runOnUiThread
                s.updateCheckedAt = System.currentTimeMillis()
                val r = res.getOrNull()
                when {
                    r != null -> ask(a, s, r)
                    auto -> {}
                    res.isFailure -> msg(a, L.t("Не удалось проверить обновления: ${res.exceptionOrNull()?.message}",
                        "Could not check for updates: ${res.exceptionOrNull()?.message}"))
                    else -> msg(a, L.t("У тебя последняя версия (${BuildConfig.VERSION_NAME}).", "You have the latest version (${BuildConfig.VERSION_NAME})."))
                }
            }
        }.start()
    }

    private fun msg(a: Activity, text: String) {
        AlertDialog.Builder(a).setMessage(text).setPositiveButton("OK", null).show()
    }

    private fun ask(a: Activity, s: Settings, r: Release) {
        val ver = Regex("([0-9.]+-\\d+)\\.apk$").find(r.asset)?.groupValues?.get(1) ?: r.tag
        val notes = r.notes.trim().take(600)
        AlertDialog.Builder(a)
            .setTitle(L.t("Доступно обновление", "Update available"))
            .setMessage(L.t("Новая версия: $ver (у тебя ${BuildConfig.VERSION_NAME}).", "New version: $ver (you have ${BuildConfig.VERSION_NAME}).") +
                (if (r.size > 0) L.t("\nРазмер: ${r.size / (1 shl 20)} МБ.", "\nSize: ${r.size / (1 shl 20)} MB.") else "") +
                (if (notes.isNotEmpty()) "\n\n$notes" else "") +
                L.t("\n\nНастройки и сохранения останутся.", "\n\nSettings and saves are kept."))
            .setPositiveButton(L.t("Скачать", "Download")) { _, _ -> runDownload(a, r) }
            .setNegativeButton(L.t("Позже", "Later")) { _, _ -> s.updateSnoozeUntil = System.currentTimeMillis() + SNOOZE_MS }
            .show()
    }

    private fun runDownload(a: Activity, r: Release) {
        val box = Ui.vbox(a, 20)
        val what = Ui.label(a, L.t("Загрузка…", "Downloading…"))
        val bar = ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal).apply { max = 1000 }
        box.addView(what)
        box.addView(bar)
        val cancelled = AtomicBoolean(false)
        val dlg = AlertDialog.Builder(a).setTitle(r.asset).setView(box).setCancelable(false)
            .setNegativeButton(L.t("Отмена", "Cancel")) { _, _ -> cancelled.set(true) }.show()
        Thread {
            var last = 0L
            val res = try {
                Result.success(download(a, r, { cancelled.get() }) { got, total ->
                    val t = System.currentTimeMillis()
                    if (t - last > 150) {
                        last = t
                        a.runOnUiThread {
                            what.text = if (total > 0) "${got / (1 shl 20)} / ${total / (1 shl 20)} " + L.t("МБ", "MB") else "${got / (1 shl 20)} " + L.t("МБ", "MB")
                            if (total > 0) bar.progress = (got * 1000 / total).toInt()
                        }
                    }
                })
            } catch (e: Exception) { Result.failure(e) }
            a.runOnUiThread {
                dlg.dismiss()
                if (cancelled.get() || a.isFinishing) return@runOnUiThread
                val f = res.getOrNull()
                if (f == null) {
                    msg(a, L.t("Не удалось скачать: ${res.exceptionOrNull()?.message}", "Download failed: ${res.exceptionOrNull()?.message}"))
                    return@runOnUiThread
                }
                if (!install(a, f)) {
                    pending = f
                    android.widget.Toast.makeText(a, L.t("Разреши установку из этого приложения и вернись назад",
                        "Allow installing from this app, then go back"), android.widget.Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    // лаунчер вернулся (из настроек разрешения): поставить скачанное
    fun resumePending(a: Activity) {
        val f = pending ?: return
        if (Build.VERSION.SDK_INT >= 26 && !a.packageManager.canRequestPackageInstalls()) return
        pending = null
        if (f.isFile) install(a, f)
    }
}

// Отдаёт системному установщику скачанный APK из кэша обновлений (content://<пакет>.apk/<имя>): FileProvider из
// androidx ради одного файла не тянем. Только чтение, только файлы каталога Updater.dir, доступ — по разовому
// разрешению из Intent (exported=false, grantUriPermissions).
class ApkProvider : ContentProvider() {
    companion object { const val MIME = "application/vnd.android.package-archive" }

    private fun file(uri: Uri): File? {
        val ctx = context ?: return null
        val name = uri.lastPathSegment ?: return null
        if (name.contains('/') || name.startsWith(".")) return null
        return File(Updater.dir(ctx), name).takeIf { it.isFile }
    }

    override fun onCreate() = true
    override fun getType(uri: Uri) = MIME
    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val f = file(uri) ?: throw java.io.FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
    }
    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? {
        val f = file(uri) ?: return null
        val cols = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        return MatrixCursor(cols).apply {
            addRow(cols.map<String, Any?> { if (it == OpenableColumns.SIZE) f.length() else if (it == OpenableColumns.DISPLAY_NAME) f.name else null }.toTypedArray())
        }
    }
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
}
