package com.webheadnx.game

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.max

// Редактор раскладки: поле с элементами (TouchControlsView в режиме правки) и панель свойств выбранного.
// Перетаскивание — переместить, двумя пальцами — размер; на панели — размер, прозрачность, видимость,
// «плавающий» стик, залипание кнопки. Используется в лаунчере (LayoutEditorActivity) и поверх игры (GameActivity).
// Панель не мешает двигать элементы под ней: её можно перетащить за заголовок, свернуть до заголовка или спрятать
// (касание пустого места поля — спрятать/показать); пока элемент тянут пальцем, панель прозрачна и не ловит касания.
class LayoutEditor(private val ctx: Context, private val settings: Settings, private val onClose: (saved: Boolean) -> Unit) {
    val field = TouchControlsView(ctx, settings, editMode = true)
    val root = FrameLayout(ctx)
    private val card = Ui.vbox(ctx, 10)            // панель: заголовок + содержимое
    private val body = Ui.vbox(ctx)                 // содержимое (сворачивается)
    private val props = Ui.vbox(ctx)
    private lateinit var scroll: ScrollView
    private lateinit var collapseBtn: TextView
    private val showBtn: TextView                   // круглая кнопка «панель», пока панель спрятана
    private var collapsed = false
    private var placed = false                      // начальное место панели выставлено (после первой разметки)

    init {
        // второй палец (размер) — тому же, кто получил первый: поле, даже если он лёг на панель
        root.isMotionEventSplittingEnabled = false
        root.addView(field, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        card.background = Ui.rounded(0xE61C2414.toInt(), Ui.dp(ctx, 14f).toFloat(), Ui.dp(ctx, 1f), 0x558DC63F)
        card.addView(header())
        scroll = object : ScrollView(ctx) {
            // не выше 60 % экрана: под панелью остаётся видно поле
            override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                val maxH = (root.height.takeIf { it > 0 } ?: resources.displayMetrics.heightPixels) * 6 / 10
                super.onMeasure(widthMeasureSpec, MeasureSpec.makeMeasureSpec(maxH, MeasureSpec.AT_MOST))
            }
        }.apply { addView(body) }
        card.addView(scroll)
        root.addView(card, FrameLayout.LayoutParams(Ui.dp(ctx, 330f), ViewGroup.LayoutParams.WRAP_CONTENT)
            .apply { gravity = Gravity.TOP or Gravity.START })
        showBtn = TextView(ctx).apply {
            text = "☰"
            textSize = 20f
            gravity = Gravity.CENTER
            setTextColor(Ui.TEXT)
            background = Ui.rounded(0xE61C2414.toInt(), Ui.dp(ctx, 24f).toFloat(), Ui.dp(ctx, 1f), 0x998DC63F.toInt())
            contentDescription = L.t("Показать панель", "Show panel")
            visibility = View.GONE
            setOnClickListener { setPanelShown(true) }
        }
        root.addView(showBtn, FrameLayout.LayoutParams(Ui.dp(ctx, 48f), Ui.dp(ctx, 48f))
            .apply { gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL; topMargin = Ui.dp(ctx, 8f) })
        // начальное место — внизу по центру; при смене размера (поворот) — обратно в пределы экрана
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            if (!placed && root.width > 0 && card.width > 0) {
                placed = true
                card.x = (root.width - card.width) / 2f
                card.y = (root.height - card.height - Ui.dp(ctx, 8f)).toFloat()
            }
            clampCard()
        }

        field.onSelect = { showProps(it) }
        field.onDrag = { dragging ->
            // элемент тянут: панель не заслоняет его и не перехватывает палец
            card.animate().alpha(if (dragging) 0.12f else 1f).setDuration(120).start()
            showBtn.alpha = if (dragging) 0.12f else 1f
        }
        field.onEmptyTap = { setPanelShown(card.visibility != View.VISIBLE) }

        body.addView(Ui.label(ctx, L.t("Тяни элемент — переместить, двумя пальцами — размер. Скрытые — пунктиром. " +
            "Панель тащи за заголовок, «▾» — свернуть, «✕» или касание пустого места — спрятать.",
            "Drag a control to move it, pinch to resize. Hidden ones are dashed. " +
            "Drag the panel by its title, “▾” collapses it, “✕” or a tap on empty space hides it."),
            dim = true, size = 12f))
        body.addView(Ui.seekRow(ctx, L.t("Прозрачность всего", "Overall opacity"), 18, ((settings.opacity - 0.1f) * 20f).toInt(),
            { "${((0.1f + it / 20f) * 100).toInt()} %" }) {
            settings.opacity = 0.1f + it / 20f
            field.invalidate()
        })
        body.addView(props)
        val buttons = Ui.hbox(ctx).apply { setPadding(0, Ui.dp(ctx, 8f), 0, 0) }
        buttons.addView(Ui.button(ctx, L.t("Сброс", "Reset")) {
            AlertDialog.Builder(ctx).setMessage(L.t("Вернуть раскладку по умолчанию?", "Restore the default layout?"))
                .setPositiveButton(L.t("Да", "Yes")) { _, _ ->
                    field.controls = TouchLayout.defaults().map { it.copy() }.toMutableList()
                    field.select(null)
                }.setNegativeButton(L.t("Нет", "No"), null).show()
        }, gap())
        buttons.addView(Ui.button(ctx, L.t("Отмена", "Cancel")) { onClose(false) }, gap())
        buttons.addView(Ui.button(ctx, L.t("Сохранить", "Save"), primary = false) { save() })
        body.addView(buttons)
        showProps(null)
    }

    private fun save() {
        TouchLayout.save(ctx, field.controls)
        onClose(true)
    }

    // заголовок: за него панель перетаскивается; «▾» — свернуть до заголовка, «✓» — сохранить, «✕» — спрятать
    @SuppressLint("ClickableViewAccessibility")
    private fun header(): View {
        val top = Ui.hbox(ctx)
        val grip = Ui.title(ctx, "⠿  " + L.t("Раскладка", "Layout"), 16f)
        top.addView(grip, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        collapseBtn = headerButton("▾", L.t("Свернуть", "Collapse")) { setCollapsed(!collapsed) }
        top.addView(collapseBtn)
        top.addView(headerButton("✓", L.t("Сохранить", "Save")) { save() })
        top.addView(headerButton("✕", L.t("Спрятать панель", "Hide panel")) { setPanelShown(false) })
        var downX = 0f; var downY = 0f; var startX = 0f; var startY = 0f
        val drag = View.OnTouchListener { _, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { downX = e.rawX; downY = e.rawY; startX = card.x; startY = card.y }
                MotionEvent.ACTION_MOVE -> {
                    card.x = startX + e.rawX - downX
                    card.y = startY + e.rawY - downY
                    clampCard()
                }
            }
            true
        }
        grip.setOnTouchListener(drag)
        top.setOnTouchListener(drag)
        return top
    }

    private fun headerButton(s: String, desc: String, onClick: () -> Unit) = TextView(ctx).apply {
        text = s
        textSize = 18f
        gravity = Gravity.CENTER
        setTextColor(Ui.TEXT)
        contentDescription = desc
        background = Ui.rounded(0xFF2B3720.toInt(), Ui.dp(ctx, 10f).toFloat())
        layoutParams = LinearLayout.LayoutParams(Ui.dp(ctx, 38f), Ui.dp(ctx, 34f)).apply { leftMargin = Ui.dp(ctx, 6f) }
        setOnClickListener { onClick() }
    }

    private fun setCollapsed(v: Boolean) {
        collapsed = v
        scroll.visibility = if (v) View.GONE else View.VISIBLE
        collapseBtn.text = if (v) "▴" else "▾"
        collapseBtn.contentDescription = if (v) L.t("Развернуть", "Expand") else L.t("Свернуть", "Collapse")
        card.post { clampCard() }
    }

    private fun setPanelShown(v: Boolean) {
        card.visibility = if (v) View.VISIBLE else View.GONE
        showBtn.visibility = if (v) View.GONE else View.VISIBLE
        if (v) card.post { clampCard() }
    }

    // панель — целиком в пределах экрана
    private fun clampCard() {
        if (root.width <= 0 || card.width <= 0) return
        card.x = card.x.coerceIn(0f, max(0f, (root.width - card.width).toFloat()))
        card.y = card.y.coerceIn(0f, max(0f, (root.height - card.height).toFloat()))
    }

    private fun gap() = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        .apply { rightMargin = Ui.dp(ctx, 8f) }

    private fun nameOf(c: Control): String = when {
        c.kind == Kind.STICK -> if (c.rightStick) L.t("Правый стик (камера)", "Right stick (camera)") else L.t("Левый стик (ходьба)", "Left stick (move)")
        c.kind == Kind.DPAD -> L.t("Крестовина", "D-pad")
        c.label.isNotEmpty() -> L.t("Кнопка «${c.label}»", "Button “${c.label}”")
        else -> c.id
    }

    private fun showProps(c: Control?) {
        props.removeAllViews()
        if (c == null) {
            props.addView(Ui.label(ctx, L.t("Выбери элемент на экране.", "Select a control on the screen."), dim = true))
            props.addView(Ui.button(ctx, L.t("Показать скрытые элементы…", "Show hidden controls…")) { pickHidden() })
            return
        }
        props.addView(Ui.title(ctx, nameOf(c), 15f))
        props.addView(Ui.switchRow(ctx, L.t("Показывать", "Visible"), null, c.visible) { c.visible = it; field.invalidate() })
        props.addView(Ui.seekRow(ctx, L.t("Размер", "Size"), 49, ((c.size - TouchLayout.MIN_SIZE) / (TouchLayout.MAX_SIZE - TouchLayout.MIN_SIZE) * 49).toInt(),
            { "${(TouchLayout.MIN_SIZE * 100 + it).toInt()}" }) {
            c.size = TouchLayout.MIN_SIZE + (TouchLayout.MAX_SIZE - TouchLayout.MIN_SIZE) * it / 49f
            field.invalidate()
        })
        props.addView(Ui.seekRow(ctx, L.t("Непрозрачность", "Opacity"), 9, ((c.opacity - 0.1f) * 10f).toInt(),
            { "${((0.1f + it / 10f) * 100).toInt()} %" }) {
            c.opacity = 0.1f + it / 10f
            field.invalidate()
        })
        if (c.kind == Kind.STICK)
            props.addView(Ui.switchRow(ctx, L.t("Плавающий", "Floating"), L.t("Появляется там, где коснулся пальцем (в своей половине экрана)", "Appears where you touch (in its half of the screen)"),
                c.floating) { c.floating = it; field.invalidate() })
        if (c.kind == Kind.BUTTON)
            props.addView(Ui.switchRow(ctx, L.t("Залипание", "Toggle"), L.t("Нажал — держится до следующего нажатия (бег, блок)", "Tap to hold until the next tap (run, block)"),
                c.toggle) { c.toggle = it })
    }

    private fun pickHidden() {
        val hidden = field.controls.filter { !it.visible }
        if (hidden.isEmpty()) return
        AlertDialog.Builder(ctx).setTitle(L.t("Скрытые элементы", "Hidden controls"))
            .setItems(hidden.map { nameOf(it) }.toTypedArray()) { _, i ->
                hidden[i].visible = true
                field.select(hidden[i])
            }.show()
    }
}

// Редактор из лаунчера: на весь экран, над тёмной сеткой
class LayoutEditorActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
        val ed = LayoutEditor(this, Settings(this)) { saved ->
            if (saved) setResult(RESULT_OK)
            finish()
        }
        setContentView(ed.root)
    }
}
