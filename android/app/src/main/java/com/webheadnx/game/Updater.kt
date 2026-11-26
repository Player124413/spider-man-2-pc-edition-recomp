package com.webheadnx.game

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import org.json.JSONArray
import java.util.zip.ZipFile
import java.net.HttpURLConnection
import java.net.URL

// Обновления из GitHub Releases (репозиторий порта): номер сборки больше своего — окно «Доступна версия…»
// с «Скачать» / «Позже». Скачивание и установка — в браузере (ссылка на APK в выпуске): приложению больше не
// нужно разрешение REQUEST_INSTALL_PACKAGES, из-за которого Google Play защита ругалась на «опасное приложение».
// Имена — как в CI
// (.github/workflows/android.yml, шаг «Имя файла»): Webhead2-0.1.0-<N>.apk — с игрой,
// webheadNX-nogame-0.1.0-<N>.apk — без; N — число коммитов (versionName), его и сравниваем. Один APK на обе
// локализации: eng/rus различаются только файлами игры, выбирать сборку не нужно.
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
            for (i in 0 until arr.length()) {
                val r = arr.getJSONObject(i)
                if (r.optBoolean("draft") || r.optBoolean("prerelease")) continue
                val assets = r.optJSONArray("assets") ?: continue
                for (j in 0 until assets.length()) {
                    val a = assets.getJSONObject(j)
                    val name = a.optString("name")
                    val m = Regex("^" + Regex.escape(prefix) + "[0-9.]+-(\\d+)\\.apk$").find(name) ?: continue
                    val build = m.groupValues[1].toInt()
                    val cur = best
                    if (cur == null || build > cur.build) {
                        best = Release(r.optString("tag_name"), r.optString("body"), name, a.optString("browser_download_url"),
                            a.optLong("size"), build)
                    }
                }
            }
            return best?.takeIf { it.build > ownBuild() && it.url.startsWith("https://") }
        } finally {
            c.disconnect()
        }
    }

    // ---------------------------------------------------------------- окна лаунчера

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
            .setPositiveButton(L.t("Скачать", "Download")) { _, _ -> openInBrowser(a, r.url) }
            .setNegativeButton(L.t("Позже", "Later")) { _, _ -> s.updateSnoozeUntil = System.currentTimeMillis() + SNOOZE_MS }
            .show()
    }

    // Скачивание и установка — браузером: он сам спросит «установка из неизвестных источников» для себя, а не для
    // нас (иначе приложению нужно разрешение REQUEST_INSTALL_PACKAGES — с ним Play защита и ругалась).
    private fun openInBrowser(a: Activity, url: String) {
        try {
            a.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        } catch (e: Exception) {
            msg(a, L.t("Не открылся браузер. Скачай APK вручную: $url", "Could not open a browser. Download the APK manually: $url"))
        }
    }
}
