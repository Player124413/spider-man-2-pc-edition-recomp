package com.shreknx.game

import android.content.Context
import java.io.File
import java.io.RandomAccessFile
import java.util.zip.CRC32

// Версия игры в папке (английская или русская) — для универсального APK (flavor uni: libmain.so — код английской
// версии, libmain_rus.so — русской). Код игры сгенерирован из DLL конкретной копии, и чужая версия не запустится
// (ошибка CRC модуля), поэтому лаунчер выбирает библиотеку по файлам игры:
// 1) CRC32 System/Core.dll против assets/variants.txt (строки «eng <crc>», «rus <crc>» — CI пишет их при сборке
//    из тех же копий игры, из которых сгенерирован код: tools/build.sh android-uni);
// 2) иначе — дата сборки Core.dll из заголовка PE: английские DLL — апрель 2004, русские («Новый Диск») —
//    декабрь 2004 (AGENTS.md, «Русская версия»).
// Ручной выбор в лаунчере («Версия игры») — поверх определения.
object GameVariant {
    const val ENG = "eng"
    const val RUS = "rus"

    class Result(val variant: String?, val how: String)

    private var cacheKey = ""
    private var cache: Result? = null

    fun isUniversal() = BuildConfig.VARIANT == "uni"

    // версия, с которой запускать игру: у обычных сборок — своя, у универсальной — выбор или определение
    fun effective(ctx: Context, s: Settings = Settings(ctx)): String {
        if (!isUniversal()) return BuildConfig.VARIANT
        return when (s.gameVariant) {
            1 -> ENG
            2 -> RUS
            else -> detect(ctx, s.gameDir).variant ?: ENG
        }
    }

    fun detect(ctx: Context, gameDir: String): Result {
        val sys = GameFolder.child(File(gameDir), "System") ?: return Result(null, "no System")
        // PE-модули у английской и русской локализации Spider-Man 2 одинаковые (CRC и дата не помогут):
        // язык определяем по System/Startup.int (в русской копии он в UTF-16 и содержит кириллицу).
        val lang = detectLanguage(sys)
        if (lang != null) return Result(lang, "Startup.int: ${if (lang == RUS) "ru" else "en"}")
        val core = GameFolder.child(sys, "Core.dll") ?: return Result(null, "no Core.dll")
        val key = "${core.absolutePath}:${core.length()}:${core.lastModified()}"
        synchronized(this) {
            if (key == cacheKey) cache?.let { return it }
        }
        val r = try {
            detectFile(ctx, core)
        } catch (e: Exception) {
            Result(null, "error: ${e.message}")
        }
        synchronized(this) {
            cacheKey = key
            cache = r
        }
        return r
    }

    private fun detectFile(ctx: Context, core: File): Result {
        val known = knownCrcs(ctx)
        if (known.isNotEmpty()) {
            val crc = crc32(core)
            known[crc]?.let { return Result(it, "CRC %08x".format(crc)) }
        }
        val t = peTimestamp(core) ?: return Result(null, "Core.dll is not a PE file")
        // граница — 2004-08-01: английские DLL от 2004-04-21, русские — от 2004-12-13
        val v = if (t >= 1091318400L) RUS else ENG
        return Result(v, "PE date ${java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.ROOT).format(java.util.Date(t * 1000))}")
    }

    private fun knownCrcs(ctx: Context): Map<Long, String> {
        val m = HashMap<Long, String>()
        try {
            ctx.assets.open("variants.txt").bufferedReader().useLines { lines ->
                for (line in lines) {
                    val p = line.trim().split(Regex("\\s+"))
                    if (p.size >= 2 && (p[0] == ENG || p[0] == RUS)) p[1].toLongOrNull(16)?.let { m[it] = p[0] }
                }
            }
        } catch (e: Exception) {
            // нет файла (сборка без игры или не универсальная) — только по дате PE
        }
        return m
    }

    // null — не удалось определить; иначе RUS/ENG по признакам кириллицы в Start/Subtitles .int
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
                    if (v in 0xC0..0xFF || v == 0xA8 || v == 0xB8) cyr++
                    else if (v in 0x41..0x7A) latin++
                }
            }
            if (cyr > 8 && cyr > latin / 4) return RUS
            if (latin > 8 && cyr == 0) return ENG
        }
        return null
    }

    private fun crc32(f: File): Long {
        val c = CRC32()
        f.inputStream().use { input ->
            val buf = ByteArray(1 shl 16)
            while (true) {
                val n = input.read(buf)
                if (n <= 0) break
                c.update(buf, 0, n)
            }
        }
        return c.value
    }

    // TimeDateStamp заголовка COFF: e_lfanew по смещению 0x3C, затем «PE\0\0», машина (2), число секций (2), дата (4)
    fun peTimestamp(f: File): Long? {
        RandomAccessFile(f, "r").use { r ->
            if (r.length() < 0x40) return null
            fun u16(): Int { val a = r.read(); val b = r.read(); return a or (b shl 8) }
            fun u32(): Long { val lo = u16().toLong(); val hi = u16().toLong(); return lo or (hi shl 16) }
            if (u16() != 0x5A4D) return null  // MZ
            r.seek(0x3C)
            val pe = u32()
            if (pe <= 0 || pe + 12 > r.length()) return null
            r.seek(pe)
            if (u32() != 0x4550L) return null  // PE\0\0
            r.seek(pe + 8)
            return u32()
        }
    }

    fun label(v: String) = if (v == RUS) L.t("русская", "Russian") else L.t("английская", "English")
}
