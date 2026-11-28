package com.webheadnx.game

import android.graphics.Path

// Разбор атрибута d из SVG (<path d="...">) в android.graphics.Path: значки экранного управления и окна «О порте»
// записаны как SVG-пути (WebIcons.kt), а Android сам SVG не читает. Команды M L H V C S Q T Z — абсолютные и
// относительные (строчные), числа через пробел или запятую, «-» и вторая «.» начинают новое число. Дуг (A) нет:
// окружности в значках — кривыми C или drawCircle.
object SvgPath {
    // приёмник команд: Path на телефоне, заглушка в проверке на ПК
    interface Sink {
        fun moveTo(x: Float, y: Float)
        fun lineTo(x: Float, y: Float)
        fun cubicTo(x1: Float, y1: Float, x2: Float, y2: Float, x: Float, y: Float)
        fun quadTo(x1: Float, y1: Float, x: Float, y: Float)
        fun close()
    }

    fun parse(d: String): Path {
        val p = Path()
        parse(d, object : Sink {
            override fun moveTo(x: Float, y: Float) = p.moveTo(x, y)
            override fun lineTo(x: Float, y: Float) = p.lineTo(x, y)
            override fun cubicTo(x1: Float, y1: Float, x2: Float, y2: Float, x: Float, y: Float) = p.cubicTo(x1, y1, x2, y2, x, y)
            override fun quadTo(x1: Float, y1: Float, x: Float, y: Float) = p.quadTo(x1, y1, x, y)
            override fun close() = p.close()
        })
        return p
    }

    fun parse(d: String, out: Sink) {
        var i = 0
        val n = d.length
        fun skip() {
            while (i < n && (d[i] == ' ' || d[i] == ',' || d[i] == '\n' || d[i] == '\t' || d[i] == '\r')) i++
        }
        fun number(): Float {
            skip()
            val start = i
            if (i < n && (d[i] == '-' || d[i] == '+')) i++
            var dot = false
            while (i < n) {
                val ch = d[i]
                if (ch.isDigit()) i++
                else if (ch == '.' && !dot) { dot = true; i++ }
                else if ((ch == 'e' || ch == 'E') && i + 1 < n && (d[i + 1].isDigit() || d[i + 1] == '-' || d[i + 1] == '+')) {
                    i += 2
                    while (i < n && d[i].isDigit()) i++
                    break
                } else break
            }
            if (start == i) throw IllegalArgumentException("SVG path: число ожидалось в позиции $start: «$d»")
            return d.substring(start, i).toFloat()
        }
        fun hasNumber(): Boolean {
            skip()
            return i < n && (d[i].isDigit() || d[i] == '-' || d[i] == '+' || d[i] == '.')
        }

        var x = 0f; var y = 0f          // текущая точка
        var sx = 0f; var sy = 0f        // начало подпути (для Z)
        var cx = 0f; var cy = 0f        // последняя контрольная точка (для S и T)
        var prev = ' '
        var cmd = ' '
        while (true) {
            skip()
            if (i >= n) break
            if (d[i].isLetter()) cmd = d[i++]
            else if (cmd == ' ') throw IllegalArgumentException("SVG path: нет команды в начале: «$d»")
            val rel = cmd.isLowerCase()
            val ox = if (rel) x else 0f
            val oy = if (rel) y else 0f
            when (cmd.uppercaseChar()) {
                'M' -> {
                    x = ox + number(); y = oy + number()
                    out.moveTo(x, y)
                    sx = x; sy = y
                    cmd = if (rel) 'l' else 'L'  // следующие пары после M — линии
                }
                'L' -> { x = ox + number(); y = oy + number(); out.lineTo(x, y) }
                'H' -> { x = ox + number(); out.lineTo(x, y) }
                'V' -> { y = oy + number(); out.lineTo(x, y) }
                'C' -> {
                    val x1 = ox + number(); val y1 = oy + number()
                    cx = ox + number(); cy = oy + number()
                    x = ox + number(); y = oy + number()
                    out.cubicTo(x1, y1, cx, cy, x, y)
                }
                'S' -> {
                    val x1 = if (prev.uppercaseChar() == 'C' || prev.uppercaseChar() == 'S') 2 * x - cx else x
                    val y1 = if (prev.uppercaseChar() == 'C' || prev.uppercaseChar() == 'S') 2 * y - cy else y
                    cx = ox + number(); cy = oy + number()
                    x = ox + number(); y = oy + number()
                    out.cubicTo(x1, y1, cx, cy, x, y)
                }
                'Q' -> {
                    cx = ox + number(); cy = oy + number()
                    x = ox + number(); y = oy + number()
                    out.quadTo(cx, cy, x, y)
                }
                'T' -> {
                    cx = if (prev.uppercaseChar() == 'Q' || prev.uppercaseChar() == 'T') 2 * x - cx else x
                    cy = if (prev.uppercaseChar() == 'Q' || prev.uppercaseChar() == 'T') 2 * y - cy else y
                    x = ox + number(); y = oy + number()
                    out.quadTo(cx, cy, x, y)
                }
                'Z' -> { out.close(); x = sx; y = sy }
                else -> throw IllegalArgumentException("SVG path: команда «$cmd» не поддерживается: «$d»")
            }
            prev = cmd
            if (cmd.uppercaseChar() == 'Z' && hasNumber())
                throw IllegalArgumentException("SVG path: числа после Z: «$d»")
        }
    }
}
