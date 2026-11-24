package com.shreknx.game

import android.graphics.Bitmap
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

// Значки зелий для кнопки «Зелья» — из файлов игры пользователя, а не из APK (в репозитории и APK нет ресурсов
// игры). Текстуры HUD: Textures/SH_Hud.utx, группа Potions — Potion1_Hud…Potion9_Hud (табличка с зельем, 256x128
// DXT5; SHGame.HudItemPotion.PotionTextures) и Potion0_Hud — та же табличка без зелья. Зелье = пиксели,
// отличающиеся от пустой таблички (+ дыры внутри контура), обрезанные по рамке. Номер текстуры = номер слота
// (клавиша цифры): порядок совпадает с меню зелий игры (SHPotionSelectMenu).
// Пакет UE2 (версия 129) читается здесь же: заголовок, имена, экспорты, теговые свойства, первый мип.
object PotionIcons {
    // имена зелий по слотам 1–9 (текстуры меню FGM_Potion_Menu.potion_menu: Strength, frog, Invisible, Sleep,
    // Stink, heroshrink, enemyshrink, Freeze, Love)
    val NAMES = arrayOf(
        "Сила" to "Strength", "Лягушка" to "Frog", "Невидимость" to "Invisibility", "Сон" to "Sleep",
        "Вонь" to "Stink", "Уменьшение" to "Shrink", "Уменьшение врагов" to "Shrink enemies",
        "Заморозка" to "Freeze", "Любовь" to "Love",
    )

    fun name(slot: Int): String = NAMES.getOrNull(slot - 1)?.let { L.t(it.first, it.second) } ?: ""

    @Volatile private var icons: Array<Bitmap?>? = null
    @Volatile private var loadedFor: String? = null
    private var loading: String? = null
    private val waiters = ArrayList<() -> Unit>()

    fun get(slot: Int): Bitmap? = icons?.getOrNull(slot - 1)

    // загрузка в фоне (один раз на папку игры); onReady — в потоке загрузки, только если значки появились
    fun load(gameDir: String, onReady: () -> Unit) {
        synchronized(this) {
            if (loadedFor == gameDir) { if (icons != null) onReady(); return }
            waiters.add(onReady)
            if (loading == gameDir) return
            loading = gameDir
        }
        Thread {
            val res = try { extract(gameDir) } catch (e: Throwable) { android.util.Log.w("snx", "potion icons: $e"); null }
            val ready: List<() -> Unit>
            synchronized(this) {
                icons = res
                loadedFor = gameDir
                loading = null
                ready = ArrayList(waiters)
                waiters.clear()
            }
            if (res != null) ready.forEach { it() }
        }.start()
    }

    private fun extract(gameDir: String): Array<Bitmap?>? {
        val tex = GameFolder.child(File(gameDir), "Textures") ?: return null
        val f = GameFolder.child(tex, "SH_Hud.utx") ?: return null
        val pkg = Pkg(f.readBytes())
        val empty = pkg.texture("Potions", "Potion0_Hud") ?: return null
        val out = arrayOfNulls<Bitmap>(9)
        for (n in 1..9) {
            val t = pkg.texture("Potions", "Potion${n}_Hud") ?: continue
            if (t.w != empty.w || t.h != empty.h) continue
            out[n - 1] = cutOut(t, empty)
        }
        return if (out.any { it != null }) out else null
    }

    // зелье = отличие от пустой таблички (порог по каналу; сжатие DXT даёт шум в блоках на краю зелья),
    // маска расширяется на пиксель, дыры внутри контура (цвет зелья похож на доску) — тоже зелье
    private fun cutOut(t: Tex, bg: Tex): Bitmap? {
        val w = t.w
        val h = t.h
        val m = BooleanArray(w * h)
        for (i in 0 until w * h) {
            val a = t.px[i]
            val b = bg.px[i]
            var d = 0
            for (sh in intArrayOf(0, 8, 16, 24)) d = maxOf(d, kotlin.math.abs((a ushr sh and 255) - (b ushr sh and 255)))
            m[i] = d > 48
        }
        val grown = m.copyOf()
        for (y in 1 until h - 1) for (x in 1 until w - 1) {
            val i = y * w + x
            if (!m[i] && (m[i - 1] || m[i + 1] || m[i - w] || m[i + w])) grown[i] = true
        }
        // заливка снаружи по «не-зелью»
        val outside = BooleanArray(w * h)
        val stack = IntArray(w * h)
        var sp = 0
        fun push(i: Int) { if (!outside[i] && !grown[i]) { outside[i] = true; stack[sp++] = i } }
        for (x in 0 until w) { push(x); push((h - 1) * w + x) }
        for (y in 0 until h) { push(y * w); push(y * w + w - 1) }
        while (sp > 0) {
            val i = stack[--sp]
            val x = i % w
            val y = i / w
            if (x > 0) push(i - 1)
            if (x < w - 1) push(i + 1)
            if (y > 0) push(i - w)
            if (y < h - 1) push(i + w)
        }
        var x0 = w; var y0 = h; var x1 = -1; var y1 = -1
        val px = IntArray(w * h)
        for (i in 0 until w * h) if (!outside[i]) {
            px[i] = t.px[i]
            val x = i % w
            val y = i / w
            if (x < x0) x0 = x
            if (x > x1) x1 = x
            if (y < y0) y0 = y
            if (y > y1) y1 = y
        }
        if (x1 < x0 || (x1 - x0) < 8 || (y1 - y0) < 8) return null
        return Bitmap.createBitmap(px, y0 * w + x0, w, x1 - x0 + 1, y1 - y0 + 1, Bitmap.Config.ARGB_8888)
    }

    class Tex(val w: Int, val h: Int, val px: IntArray)  // ARGB

    // ---------------------------------------------------------------- пакет UE2
    private class Pkg(val b: ByteArray) {
        private val bb: ByteBuffer = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
        private val names = ArrayList<String>()
        private class Export(val outer: Int, val name: String, val size: Int, val off: Int)
        private val exports = ArrayList<Export>()
        private var p = 0

        init {
            require(bb.getInt(0) == 0x9E2A83C1.toInt()) { "not an Unreal package" }
            val ver = bb.getShort(4).toInt() and 0xFFFF
            require(ver >= 68) { "package version $ver" }
            val nn = bb.getInt(12); val no = bb.getInt(16)
            val ne = bb.getInt(20); val eo = bb.getInt(24)
            p = no
            repeat(nn) {
                val n = ci()
                names.add(if (n > 0) String(b, p, n - 1, Charsets.ISO_8859_1) else "")
                p += maxOf(n, 0) + 4
            }
            p = eo
            repeat(ne) {
                ci(); ci()  // класс, родитель
                val outer = bb.getInt(p); p += 4
                val name = names[ci()]
                p += 4      // флаги
                val size = ci()
                val off = if (size > 0) ci() else 0
                exports.add(Export(outer, name, size, off))
            }
        }

        private fun ci(): Int {
            val b0 = b[p++].toInt() and 255
            var v = b0 and 0x3F
            if (b0 and 0x40 != 0) {
                var sh = 6
                while (true) {
                    val bx = b[p++].toInt() and 255
                    v = v or ((bx and 0x7F) shl sh)
                    sh += 7
                    if (bx and 0x80 == 0 || sh > 27) break
                }
            }
            return if (b0 and 0x80 != 0) -v else v
        }

        fun texture(group: String, name: String): Tex? {
            val e = exports.firstOrNull {
                it.name.equals(name, true) && it.size > 0 && it.outer > 0 && exports[it.outer - 1].name.equals(group, true)
            } ?: return null
            p = e.off
            var format = -1
            // теговые свойства UObject (Format — ByteProperty), до None
            while (true) {
                val pn = names.getOrNull(ci()) ?: return null
                if (pn == "None") break
                val info = b[p++].toInt() and 255
                val type = info and 0x0F
                if (type == 10) ci()  // имя структуры
                val size = when ((info shr 4) and 7) {
                    0 -> 1; 1 -> 2; 2 -> 4; 3 -> 12; 4 -> 16
                    5 -> (b[p++].toInt() and 255)
                    6 -> { val s = bb.getShort(p).toInt() and 0xFFFF; p += 2; s }
                    else -> { val s = bb.getInt(p); p += 4; s }
                }
                if (info and 0x80 != 0 && type != 3) p++  // индекс элемента массива
                if (pn == "Format" && type == 1) format = b[p].toInt() and 255
                if (type != 3) p += size
                if (p > e.off + e.size) return null
            }
            if (ci() <= 0) return null  // число мипов
            p += 4                       // смещение конца данных мипа (TLazyArray)
            val len = ci()
            val data = p
            if (len <= 0 || data + len + 8 > b.size) return null
            val w = bb.getInt(data + len)
            val h = bb.getInt(data + len + 4)
            if (w !in 4..2048 || h !in 4..2048) return null
            val px = when (format) {
                3 -> if (len >= w * h / 2) dxt(data, w, h, 1) else null
                7 -> if (len >= w * h) dxt(data, w, h, 3) else null
                8 -> if (len >= w * h) dxt(data, w, h, 5) else null
                5 -> if (len >= w * h * 4) IntArray(w * h) { bb.getInt(data + it * 4) } else null  // BGRA = ARGB LE
                else -> null
            } ?: return null
            return Tex(w, h, px)
        }

        private fun c565(c: Int): Int {
            val r = (c shr 11 and 31) * 255 / 31
            val g = (c shr 5 and 63) * 255 / 63
            val bl = (c and 31) * 255 / 31
            return (r shl 16) or (g shl 8) or bl
        }

        private fun mix(a: Int, b: Int, wa: Int, wb: Int, d: Int): Int {
            var o = 0
            for (sh in intArrayOf(0, 8, 16)) o = o or ((((a shr sh and 255) * wa + (b shr sh and 255) * wb) / d) shl sh)
            return o
        }

        private fun dxt(at: Int, w: Int, h: Int, kind: Int): IntArray {
            val out = IntArray(w * h)
            val bs = if (kind == 1) 8 else 16
            var q = at
            val al = IntArray(8)
            val cols = IntArray(4)
            for (by in 0 until h step 4) for (bx in 0 until w step 4) {
                var abits = 0L
                if (kind == 5) {
                    val a0 = b[q].toInt() and 255
                    val a1 = b[q + 1].toInt() and 255
                    al[0] = a0; al[1] = a1
                    if (a0 > a1) for (i in 1..6) al[i + 1] = ((7 - i) * a0 + i * a1) / 7
                    else { for (i in 1..4) al[i + 1] = ((5 - i) * a0 + i * a1) / 5; al[6] = 0; al[7] = 255 }
                    for (i in 0 until 6) abits = abits or ((b[q + 2 + i].toLong() and 255) shl (8 * i))
                } else if (kind == 3) {
                    abits = bb.getLong(q)
                }
                val cq = q + bs - 8
                val c0 = bb.getShort(cq).toInt() and 0xFFFF
                val c1 = bb.getShort(cq + 2).toInt() and 0xFFFF
                val bits = bb.getInt(cq + 4)
                cols[0] = c565(c0); cols[1] = c565(c1)
                var transparent3 = false
                if (kind != 1 || c0 > c1) {
                    cols[2] = mix(cols[0], cols[1], 2, 1, 3); cols[3] = mix(cols[0], cols[1], 1, 2, 3)
                } else {
                    cols[2] = mix(cols[0], cols[1], 1, 1, 2); cols[3] = 0; transparent3 = true
                }
                for (j in 0 until 16) {
                    val x = bx + (j and 3)
                    val y = by + (j shr 2)
                    if (x >= w || y >= h) continue
                    val ci = bits ushr (2 * j) and 3
                    val a = when (kind) {
                        5 -> al[(abits ushr (3 * j) and 7).toInt()]
                        3 -> ((abits ushr (4 * j) and 15).toInt()) * 17
                        else -> if (transparent3 && ci == 3) 0 else 255
                    }
                    out[y * w + x] = (a shl 24) or cols[ci]
                }
                q += bs
            }
            return out
        }
    }
}
