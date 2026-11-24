package com.webheadnx.game

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView

// Интерфейс лаунчера и редактора — кодом, без XML-разметки (меньше файлов, всё в одном стиле).
object Ui {
    const val BG = 0xFF0B1020.toInt()
    const val CARD = 0xFF121B2E.toInt()
    const val ACCENT = 0xFFE62429.toInt()
    const val ACCENT_DARK = 0xFF8C101C.toInt()
    const val TEXT = 0xFFF2F5FF.toInt()
    const val DIM = 0xFF9FACC4.toInt()

    fun dp(ctx: Context, v: Float) = (v * ctx.resources.displayMetrics.density + 0.5f).toInt()

    fun rounded(color: Int, radius: Float, stroke: Int = 0, strokeColor: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = radius
        if (stroke > 0) setStroke(stroke, strokeColor)
    }

    fun vbox(ctx: Context, pad: Int = 0) = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        val p = dp(ctx, pad.toFloat())
        setPadding(p, p, p, p)
    }

    fun hbox(ctx: Context) = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    fun card(ctx: Context): LinearLayout = vbox(ctx, 14).apply {
        background = rounded(CARD, dp(ctx, 14f).toFloat())
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            .apply { bottomMargin = dp(ctx, 12f) }
    }

    fun title(ctx: Context, s: String, size: Float = 18f) = TextView(ctx).apply {
        text = s
        textSize = size
        setTextColor(TEXT)
        typeface = Typeface.DEFAULT_BOLD
        setPadding(0, 0, 0, dp(ctx, 6f))
    }

    fun label(ctx: Context, s: String, dim: Boolean = false, size: Float = 14f) = TextView(ctx).apply {
        text = s
        textSize = size
        setTextColor(if (dim) DIM else TEXT)
    }

    fun button(ctx: Context, s: String, primary: Boolean = false, onClick: () -> Unit) = Button(ctx).apply {
        text = s
        isAllCaps = false
        textSize = if (primary) 18f else 14f
        setTextColor(if (primary) 0xFFFFFFFF.toInt() else TEXT)
        background = rounded(if (primary) ACCENT else 0xFF1B2947.toInt(), dp(ctx, 12f).toFloat())
        val ph = dp(ctx, 16f)
        val pv = dp(ctx, if (primary) 14f else 8f)
        setPadding(ph, pv, ph, pv)
        stateListAnimator = null
        setOnClickListener { onClick() }
    }

    fun switchRow(ctx: Context, s: String, hint: String?, value: Boolean, onChange: (Boolean) -> Unit): View {
        val box = vbox(ctx).apply { setPadding(0, dp(ctx, 4f), 0, dp(ctx, 4f)) }
        val sw = Switch(ctx).apply {
            text = s
            textSize = 15f
            setTextColor(TEXT)
            isChecked = value
            thumbTintList = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
                intArrayOf(ACCENT, 0xFFBBBBBB.toInt()))
            setOnCheckedChangeListener { _, b -> onChange(b) }
        }
        box.addView(sw)
        if (hint != null) box.addView(label(ctx, hint, dim = true, size = 12f))
        return box
    }

    // ползунок со значением: steps делений, format — подпись значения по номеру деления
    fun seekRow(ctx: Context, s: String, steps: Int, pos: Int, format: (Int) -> String, onChange: (Int) -> Unit): View {
        val box = vbox(ctx).apply { setPadding(0, dp(ctx, 4f), 0, dp(ctx, 4f)) }
        val head = hbox(ctx)
        val name = label(ctx, s, size = 15f)
        val value = label(ctx, format(pos), dim = true, size = 14f)
        head.addView(name, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(value)
        box.addView(head)
        box.addView(SeekBar(ctx).apply {
            max = steps
            progress = pos
            progressTintList = ColorStateList.valueOf(ACCENT)
            thumbTintList = ColorStateList.valueOf(ACCENT)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    value.text = format(p)
                    if (fromUser) onChange(p)
                }
                override fun onStartTrackingTouch(sb: SeekBar) {}
                override fun onStopTrackingTouch(sb: SeekBar) {}
            })
        })
        return box
    }

    // выбор одного из вариантов кнопками-«таблетками»
    fun choiceRow(ctx: Context, s: String, options: List<String>, selected: Int, onChange: (Int) -> Unit): View {
        val box = vbox(ctx).apply { setPadding(0, dp(ctx, 4f), 0, dp(ctx, 6f)) }
        box.addView(label(ctx, s, size = 15f))
        val row = hbox(ctx).apply { setPadding(0, dp(ctx, 6f), 0, 0) }
        val views = ArrayList<TextView>()
        fun paint(sel: Int) {
            for ((i, v) in views.withIndex()) {
                v.background = rounded(if (i == sel) ACCENT else 0xFF1B2947.toInt(), dp(ctx, 18f).toFloat())
                v.setTextColor(if (i == sel) 0xFFFFFFFF.toInt() else TEXT)
            }
        }
        for ((i, o) in options.withIndex()) {
            val v = TextView(ctx).apply {
                text = o
                textSize = 13f
                gravity = Gravity.CENTER
                val ph = dp(ctx, 12f); val pv = dp(ctx, 7f)
                setPadding(ph, pv, ph, pv)
                setOnClickListener { paint(i); onChange(i) }
            }
            views += v
            row.addView(v, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { rightMargin = dp(ctx, 8f) })
        }
        paint(selected)
        box.addView(HorizontalScrollView(ctx).apply {
            isHorizontalScrollBarEnabled = false
            addView(row)
        })
        return box
    }
}
