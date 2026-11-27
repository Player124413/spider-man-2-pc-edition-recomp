package com.webheadnx.game

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

// Экранное управление поверх игры (и редактор раскладки). Один полноэкранный View разбирает все пальцы сам:
// элементы (кнопки, стики, крестовина) -> виртуальный геймпад (SnxNative.pad), палец на свободном месте — камера:
// палец тянет камеру напрямую, как мышь в ПК-версии (SnxNative.look, см. порт Shrek 2).
// Кнопки «скользят»: палец, переехавший с кнопки на соседнюю, отпускает первую и жмёт вторую.
// В GUI-меню игры элементы прячутся. В главном меню на Bink-заставке касания пустого места идут в SDL,
// но кнопки остаются видимыми: именно A/крестовина выбирают «Начать новую игру», «Загрузить игру», «Настройки».
// Рисуется только при изменениях (без постоянной перерисовки — не тратит GPU и батарею).
class TouchControlsView(ctx: Context, private val settings: Settings, val editMode: Boolean) : View(ctx) {
    var controls: MutableList<Control> = TouchLayout.load(ctx)
        set(v) { field = v; releaseAll(); invalidate() }

    // игра: состояние меню и подключённый геймпад
    var hiddenByMenu = false
        set(v) { if (field != v) { field = v; releaseAll(); invalidate() } }
    var hiddenByGamepad = false
        set(v) { if (field != v) { field = v; releaseAll(); invalidate() } }
    // главное меню на ролике: тапы по пустому месту идут в SDL, но экранные кнопки не прячем
    var passThroughMenu = false
        set(v) { if (field != v) { field = v; releaseAll(); invalidate() } }
    // есть живой герой (из SnxNative.state): тап по пустому месту — камера; иначе (заставка, логотипы,
    // катсцены, меню) — виртуальная мышь. Не зависит от защёлки меню, поэтому меню работает всегда.
    var inGame = false
        set(v) { if (field != v) { field = v; releaseAll(); invalidate() } }
    var onGear: (() -> Unit)? = null
    // Кнопка «Меню» (Plus) дополнительно переключает принудительный режим меню порта: в нём джойстик
    // не двигает героя, а тапы по пустому месту выбирают пункты. Повторное нажатие возвращает управление.
    private var menuOverride = false
    var onSelect: ((Control?) -> Unit)? = null  // редактор: выбранный элемент
    var onDrag: ((Boolean) -> Unit)? = null      // редактор: элемент тянут пальцем (true) / отпустили (false)
    var onEmptyTap: (() -> Unit)? = null         // редактор: касание пустого места (без сдвига пальца)
    var selected: Control? = null
        private set

    private val density = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * density }
    private val dashed = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        pathEffect = DashPathEffect(floatArrayOf(8f * density, 6f * density), 0f)
    }
    private val grid = Paint().apply { strokeWidth = 1f; color = 0x22FFFFFF }
    private val bmpPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val nameText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
        textSize = 14f * density
    }

    init {
        // значки зелий из файлов игры (SH_Hud.utx) — в фоне; без них на кнопке прежняя колба
        PotionIcons.load(settings.gameDir) { post { invalidate() } }
    }
    private val path = Path()
    private val rect = RectF()

    // ---------------------------------------------------------------- состояние пальцев (игра)
    private enum class Bind { NONE, CONTROL, LOOK, GEAR, SLOT, MENU_TAP }
    // combo — пока палец держал «Зелья», жали другую кнопку (ZL+A и т. п.): тогда при отпускании слот не пьётся
    private class Finger(var bind: Bind = Bind.NONE, var ctl: Control? = null, var lastX: Float = 0f, var lastY: Float = 0f,
                         var startX: Float = 0f, var startY: Float = 0f, var combo: Boolean = false)
    private val fingers = HashMap<Int, Finger>()

    // Камера пальцем — как мышь в ПК-версии (и как в порте Shrek 2): палец тянет камеру ровно на столько,
    // на сколько сам сдвинулся, без стика, мёртвой зоны и «скорости от отклонения». Тянуть можно с любого места
    // свободного экрана; кнопки и стики перехватывают касание первыми, поэтому большим пальцем камера крутится
    // рядом с ними. Смещение за событие уходит в SnxNative.look (мышь DirectInput), а не в ось джойстика.
    private var lookPointer = -1
    // Камера пальцем как виртуальный правый стик (режим 0, по умолчанию): место касания — центр стика, отклонение
    // пальца от него — значение оси. Никаких накоплений и обратной связи: отклонил — камера поворачивается с этой
    // силой, отпустил — ось ровно 0. Центр «едет» за пальцем, если тянуть дальше радиуса, — как плавающий стик.
    private var lookAnchorX = 0f
    private var lookAnchorY = 0f
    private fun lookRadius() = min(w(), h()) * 0.16f

    private fun lookAxis(x: Float, y: Float) {
        var dx = x - lookAnchorX
        var dy = y - lookAnchorY
        val r = lookRadius().coerceAtLeast(1f)
        val d = hypot(dx, dy)
        if (d > r) {  // дальше радиуса — тянем центр за пальцем, отклонение остаётся полным
            val k = (d - r) / d
            lookAnchorX += dx * k
            lookAnchorY += dy * k
            dx = x - lookAnchorX
            dy = y - lookAnchorY
        }
        var vx = dx / r
        var vy = dy / r
        val len = hypot(vx, vy)
        if (len > 1f) {
            vx /= len
            vy /= len
        }
        if (len < 0.06f) {  // мягкая мёртвая зона, чтобы камера не дрожала от мелких касаний
            vx = 0f
            vy = 0f
        }
        SnxNative.camAxis(vx, vy)
    }

    private class StickState(var pointer: Int = -1, var cx: Float = 0f, var cy: Float = 0f, var vx: Float = 0f, var vy: Float = 0f)
    private val sticks = HashMap<String, StickState>()
    private val latched = HashSet<String>()      // залипшие кнопки (toggle)
    private var dpadMask = 0                      // крестовина: биты DPAD_*
    private var walkKeyDir = 0                    // запасной W/S для перед/назад, если игра не читает JoyY

    private var sentButtons = -1
    private var sentAxes = FloatArray(4) { Float.NaN }
    private val heldVk = HashSet<Int>()

    private fun w() = width.toFloat()
    private fun h() = height.toFloat()
    private fun cx(c: Control) = c.x * w()
    private fun cy(c: Control) = c.y * h()
    private fun radius(c: Control) = c.size * h() / 2f
    private fun gearRadius() = 18f * density
    private fun gearX() = 30f * density
    private fun gearY() = 30f * density

    // Виртуальная мышь (без настоящей мыши и без системного курсора): в меню тап по пустому месту — наведение и
    // щелчок в точке пальца (SnxNative.mouse), курсор рисует порт. В игре те же пальцы по пустому месту крутят
    // камеру. Экранные кнопки в меню HЕ прячутся: крестовина водит фокус, A нажимает, B — назад, старт — Esc.
    private fun active(): Boolean = editMode ||
        (!hiddenByGamepad && settings.touchEnabled)
    private fun controlsVisible(): Boolean = editMode ||
        (!hiddenByGamepad && settings.touchEnabled)
    private fun menuActive() = passThroughMenu || hiddenByMenu || menuOverride || !inGame
    private fun menuTapAt(x: Float, y: Float): Boolean = menuActive() &&
        hit(x, y, ALL_KINDS) == null && slotArrowAt(x, y) == 0 && floatingStickAt(x) == null &&
        !onGearAt(x, y)

    // ---------------------------------------------------------------- рисование
    override fun onDraw(canvas: Canvas) {
        if (editMode) drawGrid(canvas)
        val base = settings.opacity
        if (controlsVisible()) {
            for (c in controls) {
                if (!c.visible && !editMode) continue
                val a = if (c.visible) base * c.opacity else 0.25f
                when (c.kind) {
                    Kind.BUTTON -> drawButton(canvas, c, a)
                    Kind.STICK -> drawStick(canvas, c, a)
                    Kind.DPAD -> drawDpad(canvas, c, a)
                }
                if (editMode && c === selected) {
                    ring.color = 0xFFE62429.toInt()
                    canvas.drawCircle(cx(c), cy(c), radius(c) + 6f * density, dashed.apply { color = 0xFFE62429.toInt() })
                }
                if (editMode && !c.visible) {
                    dashed.color = 0x99FFFFFF.toInt()
                    canvas.drawCircle(cx(c), cy(c), radius(c), dashed)
                }
            }
        }
        if (!editMode)
            drawGear(canvas)
    }

    private fun drawGrid(canvas: Canvas) {
        canvas.drawColor(0xFF0B1633.toInt())
        var i = 1
        while (i < 20) {
            val x = w() * i / 20f
            val y = h() * i / 20f
            canvas.drawLine(x, 0f, x, h(), grid)
            canvas.drawLine(0f, y, w(), y, grid)
            i++
        }
    }

    private fun alpha(a: Float) = (a.coerceIn(0f, 1f) * 255).toInt()

    // кнопки и стики — значки в стиле Spider-Man (WebIcons: SVG-пути, паутина); у кнопок без значка — надпись
    private fun drawButton(canvas: Canvas, c: Control, a: Float) {
        val pressed = isPressed(c)
        val potion = if (c.id == POTION_ID) PotionIcons.get(settings.potionSlot) else null
        val al = alpha(if (pressed) min(1f, a * 1.2f) else a)
        WebIcons.drawButton(canvas, cx(c), cy(c), radius(c), if (potion != null) null else WebIcons.iconFor(c.id),
            if (potion != null) "" else c.label, pressed, c.toggle && c.id in latched, al)
        if (potion != null) drawPotion(canvas, c, potion, pressed, al)
        if (c.id == POTION_ID) drawSlot(canvas, c, a)
    }

    // зелье слота (значок игры) внутри круга кнопки
    private fun drawPotion(canvas: Canvas, c: Control, b: android.graphics.Bitmap, pressed: Boolean, al: Int) {
        val box = radius(c) * (if (pressed) 1.2f else 1.34f)  // сторона квадрата под значок (круг кнопки — 0.8 от r*2)
        val k = box / max(b.width, b.height)
        val bw = b.width * k
        val bh = b.height * k
        rect.set(cx(c) - bw / 2, cy(c) - bh / 2, cx(c) + bw / 2, cy(c) + bh / 2)
        bmpPaint.alpha = al
        canvas.drawBitmap(b, null, rect, bmpPaint)
    }

    // ---------------------------------------------------------------- слот зелья у кнопки «Зелья»
    // Слева и справа от кнопки — стрелки (слот 1–9 по кругу), снизу справа — номер слота. Короткое нажатие
    // «Зелья» (без других кнопок) — клавиша цифры слота, как цифры 1–9 на ПК; удержание с «Ударом»/«Прыжком» —
    // по-прежнему слой ZL (зелья 6 и 7 из Snx.ini). Какое зелье в каком слоте — решает игра.
    private var slotFlash = 0  // -1/1 — стрелка нажата (подсветка), 0 — нет

    private fun slotArrowX(c: Control, dir: Int) = cx(c) + dir * radius(c) * 1.6f
    private fun slotArrowR(c: Control) = radius(c) * 0.42f

    private fun drawSlot(canvas: Canvas, c: Control, a: Float) {
        val r = radius(c)
        val y = cy(c)
        for (dir in intArrayOf(-1, 1))
            WebIcons.drawArrow(canvas, slotArrowX(c, dir), y, slotArrowR(c), dir < 0, slotFlash == dir, alpha(a))
        WebIcons.drawBadge(canvas, cx(c) + r * 0.8f, y + r * 0.8f, r * 0.36f, settings.potionSlot.toString(), alpha(min(1f, a * 1.3f)))
        // после смены слота — название зелья над кнопкой (1,5 с)
        val left = slotNameUntil - android.os.SystemClock.uptimeMillis()
        if (left > 0 && !editMode) {
            val name = PotionIcons.name(settings.potionSlot)
            if (name.isNotEmpty()) {
                val fade = min(1f, left / 300f)
                val ty = y - r * 1.25f
                nameText.style = Paint.Style.STROKE
                nameText.strokeWidth = 3.5f * density
                nameText.color = WebIcons.SWAMP
                nameText.alpha = (fade * 230).toInt()
                canvas.drawText(name, cx(c), ty, nameText)
                nameText.style = Paint.Style.FILL
                nameText.color = WebIcons.CREAM
                nameText.alpha = (fade * 255).toInt()
                canvas.drawText(name, cx(c), ty, nameText)
                postInvalidateDelayed(if (left > 300) left - 300 else 30)
            }
        }
    }

    private var slotNameUntil = 0L

    // стрелка слота под пальцем: -1 / 1, 0 — нет
    private fun slotArrowAt(x: Float, y: Float): Int {
        val c = controls.firstOrNull { it.id == POTION_ID && it.visible } ?: return 0
        for (dir in intArrayOf(-1, 1))
            if (hypot(x - slotArrowX(c, dir), y - cy(c)) < slotArrowR(c) * 1.35f) return dir
        return 0
    }

    private fun drinkSlot() {
        val vk = 0x30 + settings.potionSlot
        try {
            SnxNative.key(vk, true)
            // клавиша держится пару кадров: игра опрашивает ввод раз в кадр
            postDelayed({ try { SnxNative.key(vk, false) } catch (e: UnsatisfiedLinkError) {} }, 90)
        } catch (e: UnsatisfiedLinkError) {
        }
    }

    private fun drawStick(canvas: Canvas, c: Control, a: Float) {
        val st = sticks[c.id]
        val activeNow = st != null && st.pointer >= 0
        if (c.floating && !activeNow && !editMode && a < 0.3f) return
        val bx = if (activeNow) st.cx else cx(c)
        val by = if (activeNow) st.cy else cy(c)
        val r = radius(c)
        val kx = bx + (st?.vx ?: 0f) * r * 0.6f
        val ky = by + (st?.vy ?: 0f) * r * 0.6f
        WebIcons.drawStick(canvas, bx, by, r, kx, ky, activeNow, a)
    }

    private fun drawDpad(canvas: Canvas, c: Control, a: Float) {
        val x = cx(c); val y = cy(c); val r = radius(c); val t = r * 0.36f
        val dirs = intArrayOf(SnxNative.DPAD_UP, SnxNative.DPAD_RIGHT, SnxNative.DPAD_DOWN, SnxNative.DPAD_LEFT)
        for ((i, bit) in dirs.withIndex()) {
            canvas.save()
            canvas.rotate(90f * i, x, y)
            rect.set(x - t, y - r, x + t, y - t * 0.6f)
            val on = dpadMask and (1 shl bit) != 0
            fill.color = if (on) 0xE62429 else 0x0B1633
            fill.alpha = alpha(if (on) a * 0.9f else a * 0.55f)
            canvas.drawRoundRect(rect, t * 0.4f, t * 0.4f, fill)
            ring.color = 0xE62429
            ring.alpha = alpha(a)
            canvas.drawRoundRect(rect, t * 0.4f, t * 0.4f, ring)
            path.reset()
            path.moveTo(x, y - r * 0.86f)
            path.lineTo(x - t * 0.45f, y - r * 0.62f)
            path.lineTo(x + t * 0.45f, y - r * 0.62f)
            path.close()
            fill.color = if (on) 0x0B1633 else 0xFFFFFF
            fill.alpha = alpha(a)
            canvas.drawPath(path, fill)
            canvas.restore()
        }
    }

    private fun drawGear(canvas: Canvas) {
        // кнопка редактора/настроек управления: всегда видна (бледно), даже в меню игры
        val x = gearX(); val y = gearY(); val r = gearRadius()
        fill.color = 0x000000
        fill.alpha = 90
        canvas.drawCircle(x, y, r, fill)
        ring.color = 0xFFFFFF
        ring.alpha = 140
        canvas.drawCircle(x, y, r * 0.45f, ring)
        for (i in 0 until 8) {
            canvas.save()
            canvas.rotate(45f * i, x, y)
            canvas.drawLine(x, y - r * 0.55f, x, y - r * 0.8f, ring)
            canvas.restore()
        }
    }

    private fun isPressed(c: Control): Boolean {
        if (c.toggle && c.id in latched) return true
        return fingers.values.any { it.bind == Bind.CONTROL && it.ctl === c }
    }

    // ---------------------------------------------------------------- касания
    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (editMode) return editTouch(e)
        val idx = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val x = e.getX(idx); val y = e.getY(idx)
                if (!onGearAt(x, y) && !menuTapAt(x, y) && !active()) return false
                down(e.getPointerId(idx), x, y)
            }
            MotionEvent.ACTION_POINTER_DOWN -> down(e.getPointerId(idx), e.getX(idx), e.getY(idx))
            // Android склеивает быстрые движения в одно событие: в нём несколько «исторических» точек, а не
            // только последняя. Без них быстрый разворот пальцем терял ход и камера «не докручивалась».
            MotionEvent.ACTION_MOVE -> {
                for (h in 0 until e.historySize)
                    for (i in 0 until e.pointerCount) move(e.getPointerId(i), e.getHistoricalX(i, h), e.getHistoricalY(i, h))
                for (i in 0 until e.pointerCount) move(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_POINTER_UP -> up(e.getPointerId(idx), e.getX(idx), e.getY(idx))
            MotionEvent.ACTION_UP -> up(e.getPointerId(idx), e.getX(idx), e.getY(idx))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        send()
        return true
    }

    private fun onGearAt(x: Float, y: Float) = hypot(x - gearX(), y - gearY()) < gearRadius() * 1.6f

    private fun hit(x: Float, y: Float, kinds: Set<Kind>): Control? {
        var best: Control? = null
        var bestD = Float.MAX_VALUE
        for (c in controls) {
            if (!c.visible || c.kind !in kinds) continue
            if (c.kind == Kind.STICK && c.floating) continue
            val d = hypot(x - cx(c), y - cy(c))
            val reach = radius(c) * (if (c.kind == Kind.BUTTON) 1.12f else 1.25f)
            if (d < reach && d / reach < bestD) { best = c; bestD = d / reach }
        }
        return best
    }

    // плавающий стик: левая (или правая — для правого стика) часть экрана, где нет других элементов
    private fun floatingStickAt(x: Float): Control? {
        val left = x < w() * 0.42f
        return controls.firstOrNull {
            it.visible && it.kind == Kind.STICK && it.floating && it.rightStick != left &&
                (sticks[it.id]?.pointer ?: -1) < 0
        }
    }

    private fun down(id: Int, x: Float, y: Float) {
        val f = Finger(lastX = x, lastY = y, startX = x, startY = y)
        fingers[id] = f
        if (onGearAt(x, y)) { f.bind = Bind.GEAR; return }
        if (menuTapAt(x, y)) {
            f.bind = Bind.MENU_TAP
            // Курсор ведём без нажатой кнопки; сам щелчок — по отпусканию, если палец не сдвинулся.
            SnxNative.mouse(x / max(1f, w()), y / max(1f, h()), -1)
            invalidate()
            return
        }
        if (!active()) return
        val dir = slotArrowAt(x, y)
        if (dir != 0) {
            f.bind = Bind.SLOT
            settings.potionSlot = (settings.potionSlot - 1 + dir + 9) % 9 + 1
            slotNameUntil = android.os.SystemClock.uptimeMillis() + 1500
            slotFlash = dir
            buzz()
            invalidate()
            return
        }
        val c = hit(x, y, ALL_KINDS)
        if (c != null) {
            f.bind = Bind.CONTROL
            f.ctl = c
            if (c.kind == Kind.BUTTON && c.id != POTION_ID) markCombo()
            if (c.id == "plus") {
                menuOverride = !menuOverride
                try { SnxNative.menuMode(menuOverride) } catch (e: UnsatisfiedLinkError) {}
                invalidate()
            }
            when (c.kind) {
                Kind.STICK -> startStick(c, id, cx(c), cy(c), x, y)
                Kind.DPAD -> { markCombo(); updateDpad(c, x, y) }
                Kind.BUTTON -> {
                    if (c.toggle) { if (!latched.remove(c.id)) latched.add(c.id) }
                    buzz()
                }
            }
        } else {
            val fs = floatingStickAt(x)
            if (fs != null) {
                f.bind = Bind.CONTROL
                f.ctl = fs
                startStick(fs, id, x, y, x, y)
            } else {
                f.bind = Bind.LOOK
                if (lookPointer < 0) {  // второй палец на пустом месте камеру не тянет
                    lookPointer = id
                    lookAnchorX = x
                    lookAnchorY = y
                    if (settings.camTouchMode == 0)
                        SnxNative.camAxis(0f, 0f)
                }
            }
        }
        invalidate()
    }

    private fun move(id: Int, x: Float, y: Float) {
        val f = fingers[id] ?: return
        when (f.bind) {
            Bind.MENU_TAP -> {
                // Курсор порт не рисует: он мешал смотреть меню. Внутренний курсор игры просто едет за пальцем,
                // чтобы на отпускании щелчок попал в пункт под пальцем.
                SnxNative.mouse(x / max(1f, w()), y / max(1f, h()), -1)
            }
            Bind.LOOK -> {
                if (id != lookPointer) return  // камеру ведёт первый палец на пустом месте
                // Камера пальцем — ОДИН жест, без единой настройки. Порт шлёт смещение в долях высоты экрана:
                // столько же градусов поворота даёт любой телефон, а сколько это отсчётов мыши, решает порт
                // (host/boot/input.c, cam_tick): он измеряет отклик камеры игры на жест и сам пересчитывает
                // доли в отсчёты так, чтобы полный жест по высоте давал ≈180°. Ползунки на палец не влияют.
                if (settings.camTouchMode == 0) {
                    lookAxis(x, y)  // виртуальный правый стик под пальцем
                    // тот же жест уходит и запасным путём: если игра перестанет читать ось стика, порт
                    // переключится на отсчёты мыши — и жест продолжит работать без провала
                    SnxNative.look((x - f.lastX) / max(1f, h()), (y - f.lastY) / max(1f, h()))
                } else {
                    SnxNative.look((x - f.lastX) / max(1f, h()), (y - f.lastY) / max(1f, h()))
                }
            }
            Bind.CONTROL -> {
                val c = f.ctl!!
                when (c.kind) {
                    Kind.STICK -> moveStick(c, x, y)
                    Kind.DPAD -> updateDpad(c, x, y)
                    Kind.BUTTON -> if (!c.toggle) {
                        // скольжение на соседнюю кнопку
                        val n = hit(x, y, BUTTONS)
                        if (n != null && n !== c && !n.toggle) {
                            f.ctl = n
                            if (n.id != POTION_ID) markCombo()
                            buzz()
                            invalidate()
                        }
                    }
                }
            }
            else -> {}
        }
        f.lastX = x
        f.lastY = y
    }

    private fun up(id: Int, x: Float, y: Float) {
        val f = fingers.remove(id) ?: return
        if (f.bind == Bind.MENU_TAP) {
            // Тап = щелчок в точке пальца. Перетаскивание = только наведение (ползунки/подсветка).
            // порог щелчка с запасом: палец при тапе всегда чуть смещается, а промах на 12 dp читался
            // как «ведение» и пункт меню не нажимался
            if (hypot(x - f.startX, y - f.startY) < 24f * density)
                SnxNative.mouse(x / max(1f, w()), y / max(1f, h()), 2)
            invalidate()
            return
        }
        if (f.bind == Bind.LOOK && lookPointer == id) {
            lookPointer = -1
            if (settings.camTouchMode == 0)
                SnxNative.camAxis(0f, 0f)  // палец убран — ось ровно 0
        }
        if (f.bind == Bind.GEAR && onGearAt(x, y)) onGear?.invoke()
        if (f.bind == Bind.SLOT) slotFlash = 0
        val c = f.ctl
        // короткое нажатие «Зелья» без других кнопок — выпить зелье слота
        if (f.bind == Bind.CONTROL && c != null && c.id == POTION_ID && !c.toggle && !f.combo) drinkSlot()
        if (c != null) {
            if (c.kind == Kind.STICK) sticks[c.id]?.let { if (it.pointer == id) { it.pointer = -1; it.vx = 0f; it.vy = 0f } }
            if (c.kind == Kind.DPAD && fingers.values.none { it.ctl === c }) dpadMask = 0
        }
        invalidate()
    }

    // палец на «Зельях» участвует в сочетании (слой ZL): слот при отпускании не пьётся
    private fun markCombo() {
        for (o in fingers.values) if (o.bind == Bind.CONTROL && o.ctl?.id == POTION_ID) o.combo = true
    }

    private fun startStick(c: Control, id: Int, ox: Float, oy: Float, x: Float, y: Float) {
        val st = sticks.getOrPut(c.id) { StickState() }
        st.pointer = id
        st.cx = ox
        st.cy = oy
        moveStick(c, x, y)
    }

    private fun moveStick(c: Control, x: Float, y: Float) {
        val st = sticks[c.id] ?: return
        val r = radius(c) * 0.6f
        var dx = (x - st.cx) / r
        var dy = (y - st.cy) / r
        val m = hypot(dx, dy)
        if (m > 1f) {
            // центр стоит на месте (и у плавающего — там, где коснулись): палец за краем — просто полный наклон
            dx /= m; dy /= m
        }
        // Прилипание к осям: палец на стекле не ведёт ровно вбок, и при «влево» шла ещё и составляющая
        // «вперёд/назад» — на карнизе Шрек вместо перебирания руками лез вверх или срывался, при ходьбе вбок —
        // уходил по диагонали. В пределах SNAP_DEG от оси — строго по оси (с той же силой); между — диагональ.
        if (dx != 0f || dy != 0f) {
            val ang = Math.toDegrees(atan2(abs(dy).toDouble(), abs(dx).toDouble()))  // 0 — вбок, 90 — вверх/вниз
            val mag = min(1f, hypot(dx, dy))
            if (ang < SNAP_DEG) { dx = if (dx < 0) -mag else mag; dy = 0f }
            else if (ang > 90 - SNAP_DEG) { dy = if (dy < 0) -mag else mag; dx = 0f }
        }
        st.vx = dx
        st.vy = dy
        invalidate()
    }

    private fun updateDpad(c: Control, x: Float, y: Float) {
        val dx = x - cx(c); val dy = y - cy(c)
        var mask = 0
        if (hypot(dx, dy) > radius(c) * 0.2f) {
            // 8 направлений: сектора по 45°
            val ang = Math.toDegrees(atan2(dy.toDouble(), dx.toDouble()))  // 0 — вправо, 90 — вниз
            val sector = (((ang + 360 + 22.5) % 360) / 45).toInt()
            mask = when (sector) {
                0 -> 1 shl SnxNative.DPAD_RIGHT
                1 -> (1 shl SnxNative.DPAD_RIGHT) or (1 shl SnxNative.DPAD_DOWN)
                2 -> 1 shl SnxNative.DPAD_DOWN
                3 -> (1 shl SnxNative.DPAD_DOWN) or (1 shl SnxNative.DPAD_LEFT)
                4 -> 1 shl SnxNative.DPAD_LEFT
                5 -> (1 shl SnxNative.DPAD_LEFT) or (1 shl SnxNative.DPAD_UP)
                6 -> 1 shl SnxNative.DPAD_UP
                else -> (1 shl SnxNative.DPAD_UP) or (1 shl SnxNative.DPAD_RIGHT)
            }
        }
        if (mask != dpadMask) {
            if (mask != 0 && (mask and dpadMask.inv()) != 0) buzz()
            dpadMask = mask
            invalidate()
        }
    }

    private fun buzz() {
        if (settings.haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
    }

    fun releaseAll() {
        for (f in fingers.values)
            if (f.bind == Bind.MENU_TAP) SnxNative.mouse(f.lastX / max(1f, w()), f.lastY / max(1f, h()), -1)
        fingers.clear()
        sticks.values.forEach { it.pointer = -1; it.vx = 0f; it.vy = 0f }
        if (lookPointer >= 0 && settings.camTouchMode == 0) SnxNative.camAxis(0f, 0f)
        lookPointer = -1
        dpadMask = 0
        slotFlash = 0
        if (!editMode) {
            latched.clear()
            send()
        }
    }

    // состояние -> игра (только при изменении)
    private fun send() {
        if (editMode) return
        var buttons = dpadMask
        val vks = HashSet<Int>()
        fun press(c: Control) {
            if (c.button >= 0) {
                buttons = buttons or (1 shl c.button)
            } else if (c.vk != 0) {
                // A в игре — левая кнопка мыши (удар). Enter в сенсорном управлении больше не шлём:
                // пункты меню выбираются виртуальной мышью.
                vks.add(c.vk)
            }
        }
        for (f in fingers.values) {
            val c = f.ctl ?: continue
            if (f.bind == Bind.CONTROL && c.kind == Kind.BUTTON && !c.toggle) press(c)
        }
        for (id in latched) controls.firstOrNull { it.id == id }?.let { press(it) }
        val ax = FloatArray(4)
        for (c in controls) {
            if (c.kind != Kind.STICK) continue
            val st = sticks[c.id] ?: continue
            val o = if (c.rightStick) 2 else 0
            if (abs(st.vx) > abs(ax[o])) ax[o] = st.vx
            if (abs(st.vy) > abs(ax[o + 1])) ax[o + 1] = st.vy
        }
        // Spider-Man 2 иногда игнорирует JoyY (влево/вправо через JoyX работает). Дублируем перед/назад
        // клавишами W/S с гистерезисом, чтобы не дребезжать у порога. Ось при этом остаётся аналоговой.
        val wy = ax[1]
        val enterKey = 0.35f
        val exitKey = 0.15f
        walkKeyDir = when {
            walkKeyDir == 0 && wy < -enterKey -> -1   // палец вверх — вперёд (JoyY в User.ini инвертирован)
            walkKeyDir == 0 && wy > enterKey -> 1
            walkKeyDir == -1 && wy < -exitKey -> -1
            walkKeyDir == 1 && wy > exitKey -> 1
            else -> 0
        }
        if (walkKeyDir < 0) vks.add(0x57) else if (walkKeyDir > 0) vks.add(0x53)

        if (buttons != sentButtons || !ax.contentEquals(sentAxes)) {
            try {
                SnxNative.pad(buttons, ax[0], ax[1], ax[2], ax[3])
            } catch (e: UnsatisfiedLinkError) {
                return
            }
            sentButtons = buttons
            sentAxes = ax
        }
        for (vk in vks) if (heldVk.add(vk)) SnxNative.key(vk, true)
        val it = heldVk.iterator()
        while (it.hasNext()) {
            val vk = it.next()
            if (vk !in vks) { SnxNative.key(vk, false); it.remove() }
        }
    }

    // ---------------------------------------------------------------- редактор
    private var dragId = -1
    private var dragDx = 0f
    private var dragDy = 0f
    private var pinchStart = 0f
    private var sizeStart = 0f

    private fun editHit(x: Float, y: Float): Control? {
        // в редакторе — и скрытые, и плавающие (за их место по умолчанию)
        var best: Control? = null
        var bestD = Float.MAX_VALUE
        for (c in controls) {
            val d = hypot(x - cx(c), y - cy(c))
            if (d < radius(c) * 1.1f && d / radius(c) < bestD) { best = c; bestD = d / radius(c) }
        }
        return best
    }

    fun select(c: Control?) {
        selected = c
        onSelect?.invoke(c)
        invalidate()
    }

    private fun editTouch(e: MotionEvent): Boolean {
        val idx = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val x = e.getX(idx); val y = e.getY(idx)
                val c = editHit(x, y)
                select(c)
                emptyDown = c == null
                downX = x; downY = y
                if (c != null) {
                    dragId = e.getPointerId(idx)
                    dragDx = cx(c) - x
                    dragDy = cy(c) - y
                    setDragging(true)
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> if (e.pointerCount == 2 && selected != null) {
                setDragging(true)
                pinchStart = hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))
                sizeStart = selected!!.size
                dragId = -1  // двумя пальцами — только размер
            }
            MotionEvent.ACTION_MOVE -> {
                if (emptyDown && hypot(e.getX(0) - downX, e.getY(0) - downY) > touchSlop) emptyDown = false
                val c = selected ?: return true
                if (e.pointerCount >= 2 && pinchStart > 0f) {
                    val d = hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))
                    c.size = (sizeStart * d / pinchStart).coerceIn(TouchLayout.MIN_SIZE, TouchLayout.MAX_SIZE)
                    onSelect?.invoke(c)
                } else if (dragId >= 0) {
                    val i = e.findPointerIndex(dragId)
                    if (i >= 0) {
                        // привязка к сетке 0.5 %: ровные ряды кнопок
                        c.x = snap(((e.getX(i) + dragDx) / w()).coerceIn(0.02f, 0.98f))
                        c.y = snap(((e.getY(i) + dragDy) / h()).coerceIn(0.03f, 0.97f))
                    }
                }
                invalidate()
            }
            MotionEvent.ACTION_POINTER_UP -> if (e.pointerCount <= 2) pinchStart = 0f
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragId = -1; pinchStart = 0f
                setDragging(false)
                if (e.actionMasked == MotionEvent.ACTION_UP && emptyDown) onEmptyTap?.invoke()
                emptyDown = false
            }
        }
        return true
    }

    private var emptyDown = false   // палец опущен на пустое место и ещё не сдвинулся
    private var downX = 0f
    private var downY = 0f
    private var dragging = false
    private val touchSlop = android.view.ViewConfiguration.get(ctx).scaledTouchSlop.toFloat()

    private fun setDragging(v: Boolean) {
        if (v == dragging) return
        dragging = v
        onDrag?.invoke(v)
    }

    private fun snap(v: Float) = Math.round(v * 200f) / 200f

    companion object {
        private val ALL_KINDS = setOf(Kind.BUTTON, Kind.STICK, Kind.DPAD)
        private const val POTION_ID = "zl"
        private val BUTTONS = setOf(Kind.BUTTON)
        private const val SNAP_DEG = 25.0  // стик: ближе к оси — строго по оси
    }
}
