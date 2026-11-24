package com.webheadnx.game

import java.io.File

// Один APK на обе локализации: сгенерированный код общий (PE-модули у eng и rus одинаковые), язык игры
// берётся из файлов игры. Лаунчер определяет его по System/*.int и хранит сохранения eng/rus раздельно.
// CRC и дата PE здесь не используются: они одинаковы у обеих локализаций Spider-Man 2 PC.
object GameVariant {
    const val ENG = "eng"
    const val RUS = "rus"

    class Result(val variant: String?, val how: String)

    fun effective(s: Settings): String = detect(s.gameDir).variant ?: ENG

    fun detect(gameDir: String): Result {
        val sys = GameFolder.child(File(gameDir), "System") ?: return Result(null, "no System")
        // В русской копии Startup.int — UTF-16 с кириллицей; в английской — латиница. Язык интерфейса
        // самого лаунчера выбирается отдельно — по языку телефона (L.kt).
        val lang = detectLanguage(sys) ?: return Result(null, "no language in System/*.int")
        return Result(lang, "System/*.int: ${if (lang == RUS) "ru" else "en"}")
    }

    // null — определить не удалось; иначе RUS/ENG по признакам кириллицы в .int
    private fun detectLanguage(sys: File): String? {
        for (name in listOf("Startup.int", "Webhead.int", "Subtitles.int")) {
            val f = GameFolder.child(sys, name) ?: continue
            val b = try { f.readBytes() } catch (e: Exception) { continue }
            if (b.isEmpty()) continue
            var cyr = 0
            var latin = 0
            if (b[0] == 0xFF.toByte() && b.getOrNull(1) == 0xFE.toByte()) {
                var i = 2
                while (i + 1 < b.size) {
                    val u = (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)
                    if (u in 0x0410..0x044F || u == 0x0401 || u == 0x0451) cyr++
                    else if (u in 0x41..0x7A) latin++
                    i += 2
                }
            } else {
                for (v in b) {
                    // Byte в Kotlin знаковый: приводим к беззнаковому, иначе 0xA8/0xB8 — отрицательные
                    val u = v.toInt() and 0xFF
                    if (u in 0xC0..0xFF || u == 0xA8 || u == 0xB8) cyr++
                    else if (u in 0x41..0x7A) latin++
                }
            }
            if (cyr > 8 && cyr > latin / 4) return RUS
            if (latin > 8 && cyr == 0) return ENG
        }
        return null
    }

    fun label(v: String) = if (v == RUS) L.t("русская", "Russian") else L.t("английская", "English")
}
