package com.webheadnx.game

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.Shader

// Значки экранного управления в стилистике Spider-Man: красная паутина, синяя маска, белые глаза.
// Значки — SVG-пути (SvgPath) в квадрате 100x100: круг кнопки — центр (50, 50), радиус 40. Те же пути
// можно открыть как SVG (<svg viewBox="-10 -10 120 120">) и поправить в любом редакторе.
object WebIcons {
    private enum class Mode { FILL, LINE, CUT, CUTFILL }
    private class El(val mode: Mode, val path: Path, val width: Float = 2f)

    private fun fill(d: String) = El(Mode.FILL, SvgPath.parse(d))
    private fun line(d: String, w: Float) = El(Mode.LINE, SvgPath.parse(d), w)
    private fun cut(d: String, w: Float) = El(Mode.CUT, SvgPath.parse(d), w)
    private fun cutFill(d: String) = El(Mode.CUTFILL, SvgPath.parse(d))

    // паутина поверх круга: радиальные нити и дуги между ними
    private val WEB_SPOKES = SvgPath.parse(
        "M50 50 L50 10 M50 50 L75 14 M50 50 L86 40 M50 50 L86 66 M50 50 L72 86 " +
        "M50 50 L28 86 M50 50 L14 66 M50 50 L14 40 M50 50 L25 14")
    private val WEB_RINGS = SvgPath.parse(
        "M35 22 Q50 28 65 22 M22 34 Q50 44 78 34 M18 50 Q50 62 82 50 M24 68 Q50 80 76 68")

    // значки по id элемента раскладки (TouchLayout); у остальных — надпись
    private val ICONS: Map<String, List<El>> by lazy {
        mapOf(
            // удар — кулак
            "attack" to listOf(
                fill("M31 47 C31 41 35 38 39.5 39.5 C40.5 35 45.5 33.5 49 36 C51 32.5 56.5 32.5 58.5 36.5 C62 34.5 67 36.5 " +
                    "67.5 41.5 C70 42.5 71 45 71 48 L71 58 C71 67 64.5 73 56 73 L46 73 C37 73 31 67 31 58 Z"),
                cut("M40 41 L40 50 M49.5 38 L49.5 49 M58.5 39 L58.5 49", 3f),
                cut("M31 53 C38 50 45 51 50 55 C47 59 42 61 37 61", 3f)),
            // прыжок — стрелка вверх
            "jump" to listOf(
                fill("M50 22 L72 46 L59 46 L59 63 L41 63 L41 46 L28 46 Z"),
                fill("M35 69 L65 69 C67.5 69 67.5 76 65 76 L35 76 C32.5 76 32.5 69 35 69 Z")),
            // зелья — колба с волной
            "potion" to listOf(
                fill("M43 22 L57 22 C58.5 22 58.5 28 57 28 L55 28 L55 37 C64.5 40 71 48.5 71 58 C71 69.5 61.5 77 50 77 " +
                    "C38.5 77 29 69.5 29 58 C29 48.5 35.5 40 45 37 L45 28 L43 28 C41.5 28 41.5 22 43 22 Z"),
                cutFill("M33.5 57 C40 53 45 61 50 58 C55 54.5 61 60 66.5 57 C65.5 67 59 73 50 73 C41 73 34.5 67 33.5 57 Z"),
                cut("M60 45 C62.5 46.5 64.5 49 65.5 52", 3f)),
            "menu" to listOf(line("M33 34 L67 34 M33 50 L67 50 M33 66 L67 66", 8f)),
            // центр камеры — прицел
            "center" to listOf(
                line("M50 26 L50 36 M50 64 L50 74 M26 50 L36 50 M64 50 L74 50", 6f),
                line("M50 37 C57.2 37 63 42.8 63 50 C63 57.2 57.2 63 50 63 C42.8 63 37 57.2 37 50 C37 42.8 42.8 37 50 37 Z", 5f)),
            "esc" to listOf(line("M36 36 L64 64 M64 36 L36 64", 8f)),
            "enter" to listOf(line("M66 30 L66 52 C66 56 63 58 59 58 L34 58 M44 47 L33 58 L44 69", 7f)),
        )
    }

    fun iconFor(id: String): String? = when (id) {
        "a" -> "attack"
        "b" -> "jump"
        "zl" -> "potion"
        "plus" -> "menu"
        "rs" -> "center"
        "esc" -> "esc"
        "enter" -> "enter"
        else -> null
    }

    // цвета маски: красный паук, синий костюм, тёмный фон, белые глаза
    const val RED = 0xFFE62429.toInt()
    const val BLUE = 0xFF2451A8.toInt()
    const val NAVY = 0xFF0B1633.toInt()
    const val INK = 0xFFFFFFFF.toInt()
    // совместимость со старыми вызовами (TouchControlsView)
    const val SWAMP = NAVY
    const val CREAM = INK
    private const val RED_DARK = 0xFF8C101C.toInt()
    private const val PRESSED_INK = 0xFF0B1633.toInt()

    private val baseShader = RadialGradient(50f, 40.4f, 49.6f, BLUE, NAVY, Shader.TileMode.CLAMP)
    private val pressedShader = RadialGradient(50f, 40.4f, 49.6f, RED, RED_DARK, Shader.TileMode.CLAMP)

    private val pFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val pStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val pText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    // паутина вместо «ушей»: тонкие красные нити поверх кнопки
    fun drawWeb(canvas: Canvas, angle: Float = 0f, color: Int = RED) {
        canvas.save()
        canvas.rotate(angle, 50f, 50f)
        pFill.shader = null
        pStroke.color = color
        pStroke.strokeWidth = 1.6f
        canvas.drawPath(WEB_SPOKES, pStroke)
        pStroke.strokeWidth = 1.3f
        canvas.drawPath(WEB_RINGS, pStroke)
        canvas.restore()
    }

    private fun begin(canvas: Canvas, x: Float, y: Float, r: Float, alpha: Int): Int {
        val s = r / 40f
        val save = canvas.saveLayerAlpha(x - r * 1.5f, y - r * 1.5f, x + r * 1.5f, y + r * 1.2f, alpha.coerceIn(0, 255))
        canvas.translate(x - 50f * s, y - 50f * s)
        canvas.scale(s, s)
        return save
    }

    // круглая кнопка: (x, y) — центр, r — радиус в пикселях; label — если у кнопки нет значка
    fun drawButton(canvas: Canvas, x: Float, y: Float, r: Float, icon: String?, label: String, pressed: Boolean,
                   latched: Boolean, alpha: Int) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        pFill.color = Color.WHITE
        pFill.shader = if (pressed) pressedShader else baseShader
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pFill.shader = null
        drawWeb(canvas, 0f, if (pressed) INK else RED)
        pStroke.color = RED
        pStroke.strokeWidth = 3.5f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pStroke.color = 0x40000000
        pStroke.strokeWidth = 2f
        canvas.drawCircle(50f, 50f, 35.5f, pStroke)
        if (latched) {
            pStroke.color = INK
            pStroke.strokeWidth = 3f
            canvas.drawCircle(50f, 50f, 33f, pStroke)
        }
        val ink = if (pressed) PRESSED_INK else INK
        val cutColor = if (pressed) RED else NAVY
        val els = icon?.let { ICONS[it] }
        if (els != null) {
            for (e in els) when (e.mode) {
                Mode.FILL -> {
                    pFill.color = ink
                    canvas.drawPath(e.path, pFill)
                    pStroke.color = ink
                    pStroke.strokeWidth = e.width
                    canvas.drawPath(e.path, pStroke)
                }
                Mode.LINE -> { pStroke.color = ink; pStroke.strokeWidth = e.width; canvas.drawPath(e.path, pStroke) }
                Mode.CUT -> { pStroke.color = cutColor; pStroke.strokeWidth = e.width; canvas.drawPath(e.path, pStroke) }
                Mode.CUTFILL -> { pFill.color = cutColor; canvas.drawPath(e.path, pFill) }
            }
        } else if (label.isNotEmpty()) {
            pText.color = ink
            pText.textSize = if (label.length <= 2) 34f else 19f
            val tw = pText.measureText(label)
            if (tw > 64f) pText.textSize *= 64f / tw
            canvas.drawText(label, 50f, 50f - (pText.descent() + pText.ascent()) / 2, pText)
        }
        canvas.restoreToCount(save)
    }

    // глаза маски
    private val EYE_L = SvgPath.parse("M28 46 C34 38 44 39 48 46 C43 52 34 52 28 46 Z")
    private val EYE_R = SvgPath.parse("M52 46 C56 39 66 38 72 46 C66 52 57 52 52 46 Z")

    // маска: центр (x, y), радиус r
    fun drawHead(canvas: Canvas, x: Float, y: Float, r: Float, alpha: Int, active: Boolean) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        pFill.shader = null
        pFill.color = if (active) RED else BLUE
        canvas.drawCircle(50f, 50f, 40f, pFill)
        drawWeb(canvas, 0f, if (active) INK else RED)
        pStroke.color = RED
        pStroke.strokeWidth = 4f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pFill.color = INK
        canvas.drawPath(EYE_L, pFill)
        canvas.drawPath(EYE_R, pFill)
        canvas.restoreToCount(save)
    }

    private val CHEVRON = SvgPath.parse("M45 17 L50 12 L55 17")

    // стик: основание в (bx, by) радиуса r, головка — маска в (kx, ky)
    fun drawStick(canvas: Canvas, bx: Float, by: Float, r: Float, kx: Float, ky: Float, active: Boolean, a: Float) {
        if (r <= 0f) return
        val s = r / 40f
        canvas.save()
        canvas.translate(bx - 50f * s, by - 50f * s)
        canvas.scale(s, s)
        pFill.shader = null
        pFill.color = NAVY
        pFill.alpha = (a * 0.5f * 255).toInt().coerceIn(0, 255)
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pStroke.color = BLUE
        pStroke.alpha = (a * 255).toInt().coerceIn(0, 255)
        pStroke.strokeWidth = 3f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pStroke.color = RED
        pStroke.alpha = (a * 0.9f * 255).toInt().coerceIn(0, 255)
        pStroke.strokeWidth = 2.5f
        for (i in 0 until 4) {
            canvas.save()
            canvas.rotate(90f * i, 50f, 50f)
            canvas.drawPath(CHEVRON, pStroke)
            canvas.restore()
        }
        canvas.restore()
        pFill.alpha = 255
        pStroke.alpha = 255
        drawHead(canvas, kx, ky, r * 0.38f, (a * 0.95f * 255).toInt(), active)
    }

    private val ARROW_RIGHT = SvgPath.parse("M42 30 L66 50 L42 70 C38 73 35 71 35 66 L35 34 C35 29 38 27 42 30 Z")

    fun drawArrow(canvas: Canvas, x: Float, y: Float, r: Float, left: Boolean, pressed: Boolean, alpha: Int) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        pFill.color = Color.WHITE
        pFill.shader = if (pressed) pressedShader else baseShader
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pFill.shader = null
        pStroke.color = RED
        pStroke.strokeWidth = 5f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        if (left) canvas.scale(-1f, 1f, 50f, 50f)
        canvas.translate(3f, 0f)
        pFill.color = if (pressed) PRESSED_INK else INK
        canvas.drawPath(ARROW_RIGHT, pFill)
        canvas.restoreToCount(save)
    }

    // номер слота: белый кружок с тёмной цифрой и красной каймой
    fun drawBadge(canvas: Canvas, x: Float, y: Float, r: Float, text: String, alpha: Int) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        pFill.shader = null
        pFill.color = INK
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pStroke.color = RED
        pStroke.strokeWidth = 7f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pText.color = PRESSED_INK
        pText.textSize = 56f
        canvas.drawText(text, 50f, 50f - (pText.descent() + pText.ascent()) / 2, pText)
        canvas.restoreToCount(save)
    }
}
