package com.shreknx.game

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.Shader

// Значки экранного управления «в стиле Шрека»: болотно-зелёная кнопка с ушами-трубочками огра и кремовым значком.
// Значки — SVG-пути (SvgPath) в квадрате 100x100: круг кнопки — центр (50, 50), радиус 40 (уши выступают за него
// вверх, касание по-прежнему ловится кругом). Те же пути можно открыть как SVG (<svg viewBox="-10 -10 120 120">)
// и поправить в любом редакторе. Рисуется в координатах квадрата: canvas.translate + scale, пути и шейдеры
// создаются один раз.
object ShrekIcons {
    private enum class Mode { FILL, LINE, CUT, CUTFILL }
    private class El(val mode: Mode, val path: Path, val width: Float = 2f)

    private fun fill(d: String) = El(Mode.FILL, SvgPath.parse(d))
    private fun line(d: String, w: Float) = El(Mode.LINE, SvgPath.parse(d), w)
    private fun cut(d: String, w: Float) = El(Mode.CUT, SvgPath.parse(d), w)
    private fun cutFill(d: String) = El(Mode.CUTFILL, SvgPath.parse(d))

    // ухо огра: трубочка вверх с раструбом, основание под ободом кнопки; поворачивается на ±40° вокруг центра
    private val EAR = SvgPath.parse("M43.5 16 C44.5 11 45.5 6 45.5 2 C42.5 0.5 40.5 -2.5 41 -5.5 C45.5 -8 54.5 -8 59 -5.5 " +
        "C59.5 -2.5 57.5 0.5 54.5 2 C54.5 6 55.5 11 56.5 16 Z")
    private val EAR_HOLE = SvgPath.parse("M44.5 -5 C48 -6.8 52 -6.8 55.5 -5 C52 -3.4 48 -3.4 44.5 -5 Z")

    // значки по id элемента раскладки (TouchLayout); у остальных — надпись
    private val ICONS: Map<String, List<El>> by lazy {
        mapOf(
            // удар — кулак огра
            "attack" to listOf(
                fill("M31 47 C31 41 35 38 39.5 39.5 C40.5 35 45.5 33.5 49 36 C51 32.5 56.5 32.5 58.5 36.5 C62 34.5 67 36.5 " +
                    "67.5 41.5 C70 42.5 71 45 71 48 L71 58 C71 67 64.5 73 56 73 L46 73 C37 73 31 67 31 58 Z"),
                cut("M40 41 L40 50 M49.5 38 L49.5 49 M58.5 39 L58.5 49", 3f),
                cut("M31 53 C38 50 45 51 50 55 C47 59 42 61 37 61", 3f)),
            // прыжок — стрелка вверх над землёй
            "jump" to listOf(
                fill("M50 22 L72 46 L59 46 L59 63 L41 63 L41 46 L28 46 Z"),
                fill("M35 69 L65 69 C67.5 69 67.5 76 65 76 L35 76 C32.5 76 32.5 69 35 69 Z")),
            // зелья — колба с волной зелья и бликом
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

    // id раскладки -> значок
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

    // цвета: огр, болото, крем (рубаха Шрека)
    const val OGRE = 0xFF8DC63F.toInt()
    const val OGRE_LIGHT = 0xFFB5E05A.toInt()
    const val OGRE_DARK = 0xFF2C4010.toInt()
    const val SWAMP = 0xFF16210A.toInt()
    const val CREAM = 0xFFF2EBC9.toInt()
    private const val EAR_HOLE_COLOR = 0xFF3A5216.toInt()
    private const val PRESSED_EDGE = 0xFF6E9E26.toInt()
    private const val PRESSED_INK = 0xFF1B2A0B.toInt()

    private val baseShader = RadialGradient(50f, 40.4f, 49.6f, 0xFF3F5C1C.toInt(), SWAMP, Shader.TileMode.CLAMP)
    private val pressedShader = RadialGradient(50f, 40.4f, 49.6f, OGRE_LIGHT, PRESSED_EDGE, Shader.TileMode.CLAMP)

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

    // уши огра в координатах квадрата 100x100 вокруг круга (cx, cy) радиуса 40
    fun drawEars(canvas: Canvas, angle: Float = 40f, fill: Int = OGRE, outline: Int = OGRE_DARK, hole: Int = EAR_HOLE_COLOR) {
        for (a in floatArrayOf(-angle, angle)) {
            canvas.save()
            canvas.rotate(a, 50f, 50f)
            pFill.shader = null
            pFill.color = fill
            canvas.drawPath(EAR, pFill)
            pStroke.color = outline
            pStroke.strokeWidth = 2f
            canvas.drawPath(EAR, pStroke)
            pFill.color = hole
            canvas.drawPath(EAR_HOLE, pFill)
            canvas.restore()
        }
    }

    private fun begin(canvas: Canvas, x: Float, y: Float, r: Float, alpha: Int): Int {
        val s = r / 40f
        // слой — чтобы прозрачность была общей (основания ушей не просвечивали сквозь круг)
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
        drawEars(canvas)
        pFill.color = Color.WHITE
        pFill.shader = if (pressed) pressedShader else baseShader
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pFill.shader = null
        pStroke.color = OGRE
        pStroke.strokeWidth = 3.5f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pStroke.color = 0x40000000
        pStroke.strokeWidth = 2f
        canvas.drawCircle(50f, 50f, 35.5f, pStroke)
        if (latched) {  // залипшая кнопка (toggle): светлое внутреннее кольцо
            pStroke.color = CREAM
            pStroke.strokeWidth = 3f
            canvas.drawCircle(50f, 50f, 33f, pStroke)
        }
        val ink = if (pressed) PRESSED_INK else CREAM
        val cutColor = if (pressed) PRESSED_EDGE else SWAMP
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
            if (tw > 64f) pText.textSize *= 64f / tw  // длинная надпись — по ширине круга
            canvas.drawText(label, 50f, 50f - (pText.descent() + pText.ascent()) / 2, pText)
        }
        canvas.restoreToCount(save)
    }

    // лицо огра (брови, глаза, нос, улыбка) в квадрате 100x100
    private val FACE_BROWS = SvgPath.parse("M28 38 C34 33 40 33 45 37 M55 37 C60 33 66 33 72 38")
    private val FACE_NOSE = SvgPath.parse("M44 58 C47 61 53 61 56 58")
    private val FACE_SMILE = SvgPath.parse("M33 66 C42 74 58 74 67 66")

    // голова Шрека: центр (x, y), радиус головы r (без ушей)
    fun drawHead(canvas: Canvas, x: Float, y: Float, r: Float, alpha: Int, active: Boolean) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        drawEars(canvas, 58f)
        pFill.shader = null
        pFill.color = if (active) OGRE_LIGHT else OGRE
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pStroke.color = OGRE_DARK
        pStroke.strokeWidth = 4f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pStroke.strokeWidth = 5f
        canvas.drawPath(FACE_BROWS, pStroke)
        pFill.color = OGRE_DARK
        canvas.drawCircle(38f, 47f, 5f, pFill)
        canvas.drawCircle(62f, 47f, 5f, pFill)
        pStroke.strokeWidth = 4f
        canvas.drawPath(FACE_NOSE, pStroke)
        pStroke.strokeWidth = 4.5f
        canvas.drawPath(FACE_SMILE, pStroke)
        canvas.restoreToCount(save)
    }

    private val CHEVRON = SvgPath.parse("M45 17 L50 12 L55 17")

    // стик: основание в (bx, by) радиуса r, головка — голова Шрека в (kx, ky)
    fun drawStick(canvas: Canvas, bx: Float, by: Float, r: Float, kx: Float, ky: Float, active: Boolean, a: Float) {
        if (r <= 0f) return
        val s = r / 40f
        canvas.save()
        canvas.translate(bx - 50f * s, by - 50f * s)
        canvas.scale(s, s)
        pFill.shader = null
        pFill.color = SWAMP
        pFill.alpha = (a * 0.5f * 255).toInt().coerceIn(0, 255)
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pStroke.color = OGRE
        pStroke.alpha = (a * 255).toInt().coerceIn(0, 255)
        pStroke.strokeWidth = 3f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pStroke.color = CREAM
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

    // стрелка выбора слота зелья: маленькая болотная кнопка с кремовым треугольником (влево или вправо)
    private val ARROW_RIGHT = SvgPath.parse("M42 30 L66 50 L42 70 C38 73 35 71 35 66 L35 34 C35 29 38 27 42 30 Z")

    fun drawArrow(canvas: Canvas, x: Float, y: Float, r: Float, left: Boolean, pressed: Boolean, alpha: Int) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        pFill.color = Color.WHITE
        pFill.shader = if (pressed) pressedShader else baseShader
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pFill.shader = null
        pStroke.color = OGRE
        pStroke.strokeWidth = 5f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        if (left) canvas.scale(-1f, 1f, 50f, 50f)
        canvas.translate(3f, 0f)  // треугольник визуально по центру
        pFill.color = if (pressed) PRESSED_INK else CREAM
        canvas.drawPath(ARROW_RIGHT, pFill)
        canvas.restoreToCount(save)
    }

    // номер слота: кремовый кружок с тёмной цифрой и зелёной каймой
    fun drawBadge(canvas: Canvas, x: Float, y: Float, r: Float, text: String, alpha: Int) {
        if (r <= 0f) return
        val save = begin(canvas, x, y, r, alpha)
        pFill.shader = null
        pFill.color = CREAM
        canvas.drawCircle(50f, 50f, 40f, pFill)
        pStroke.color = OGRE_DARK
        pStroke.strokeWidth = 7f
        canvas.drawCircle(50f, 50f, 40f, pStroke)
        pText.color = PRESSED_INK
        pText.textSize = 56f
        canvas.drawText(text, 50f, 50f - (pText.descent() + pText.ascent()) / 2, pText)
        canvas.restoreToCount(save)
    }
}
