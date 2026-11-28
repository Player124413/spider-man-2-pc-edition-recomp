package com.webheadnx.game

import android.app.Activity
import android.app.Dialog
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

// Окно «О порте» (первый запуск, обновление, кнопка «О порте»): компактная карточка в цветах Человека-паука
// (тёмно-синий фон, красная рамка и красный значок с маской), сверху на рамке — значок; заголовок, автор порта,
// Telegram-канал, благодарность автору дизайна окна (@base64E), кнопки «OK» / «Открыть канал». Строки — по языку (L.t).
object CreditsDialog {
    // цвета Человека-паука: красный костюм и синий
    private const val CARD_BG = 0xFF101828.toInt()
    private const val CARD_LINE = 0xFF8C1319.toInt()
    private const val BOX_BLUE = 0xFF16213C.toInt()
    private const val BOX_BLUE_LINE = 0xFF345991.toInt()
    private const val BOX_DARK = 0xFF131B2E.toInt()
    private const val BOX_DARK_LINE = 0xFF232F4A.toInt()
    private const val GREY = 0xFFA8B2C8.toInt()
    private const val BLUE = 0xFF7FB0FF.toInt()
    private const val RED = 0xFFFF5A5F.toInt()
    private const val BTN_TEXT = 0xFFF4F7FF.toInt()

    fun show(act: Activity) {
        fun dp(v: Float) = Ui.dp(act, v)
        val d = Dialog(act)
        d.requestWindowFeature(Window.FEATURE_NO_TITLE)
        fun openChannel() {
            try {
                act.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(L.TG_CHANNEL)))
            } catch (e: Exception) {
            }
        }

        // карточка
        val card = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(CARD_BG)
                cornerRadius = dp(16f).toFloat()
                setStroke(dp(1.2f), CARD_LINE)
            }
            setPadding(dp(12f), dp(26f), dp(12f), dp(6f))
        }
        // заголовок: «Spider-Man 2» белым жирным, « · webheadNX Android» синим
        card.addView(TextView(act).apply {
            val sb = SpannableStringBuilder()
            sb.append("Spider-Man 2", StyleSpan(Typeface.BOLD), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            val st = sb.length
            sb.append(" · webheadNX Android")
            sb.setSpan(ForegroundColorSpan(BLUE), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text = sb
            setTextColor(Color.WHITE)
            textSize = 15.5f
            gravity = Gravity.CENTER
        }, lp(bottom = dp(7f)))

        // автор порта: имя красным жирным
        card.addView(TextView(act).apply {
            val sb = SpannableStringBuilder(L.t("Android-порт сделан ", "Android port by "))
            val st = sb.length
            sb.append(L.AUTHOR)
            sb.setSpan(StyleSpan(Typeface.BOLD), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.setSpan(ForegroundColorSpan(RED), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text = sb
            setTextColor(Color.WHITE)
            textSize = 12.5f
            gravity = Gravity.CENTER
            background = box(act, BOX_BLUE, BOX_BLUE_LINE)
            setPadding(dp(8f), dp(6f), dp(8f), dp(6f))
        }, lp(bottom = dp(6f)))

        // Telegram-канал: 📢, подпись капсом, ссылка синим; нажатие — открыть канал
        val tg = LinearLayout(act).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = box(act, BOX_DARK, BOX_DARK_LINE)
            setPadding(dp(9f), dp(5f), dp(9f), dp(5f))
            isClickable = true
            setOnClickListener { openChannel() }
        }
        tg.addView(TextView(act).apply { text = "📢"; textSize = 16f }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(8f) })
        val tgText = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        tgText.addView(TextView(act).apply {
            text = L.t("TELEGRAM-КАНАЛ", "TELEGRAM CHANNEL")
            setTextColor(GREY)
            textSize = 9f
            letterSpacing = 0.08f
        })
        tgText.addView(TextView(act).apply {
            text = L.TG_CHANNEL.removePrefix("https://")
            setTextColor(BLUE)
            textSize = 12f
            setTypeface(typeface, Typeface.BOLD)
        })
        tg.addView(tgText)
        card.addView(tg, lp(bottom = dp(6f)))

        // благодарность за дизайн окна: ник — красный жирный
        card.addView(TextView(act).apply {
            val sb = SpannableStringBuilder(L.t("Спасибо ", "Thanks to "))
            val st = sb.length
            sb.append(L.DESIGNER)
            sb.setSpan(StyleSpan(Typeface.BOLD), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.setSpan(ForegroundColorSpan(RED), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.append(L.t(" за редизайн всплывающего окна.", " for the redesign of this pop-up window."))
            text = sb
            setTextColor(GREY)
            textSize = 11f
            setPadding(dp(4f), 0, dp(4f), dp(2f))
        }, lp(bottom = dp(6f)))

        // разделитель
        card.addView(View(act).apply { setBackgroundColor(BOX_DARK_LINE) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1f)).apply { bottomMargin = dp(4f) })

        // кнопки: «OK» — текстом, «Открыть канал» — красная «таблетка» со значком Telegram
        val buttons = LinearLayout(act).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL or Gravity.END
        }
        buttons.addView(TextView(act).apply {
            text = "OK"
            setTextColor(0xFFB6C0D4.toInt())
            textSize = 12f
            setTypeface(typeface, Typeface.BOLD)
            gravity = Gravity.CENTER
            setPadding(dp(12f), dp(6f), dp(12f), dp(6f))
            isClickable = true
            setOnClickListener { d.dismiss() }
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(6f) })
        buttons.addView(TextView(act).apply {
            text = L.t("ОТКРЫТЬ КАНАЛ", "OPEN CHANNEL")
            setTextColor(BTN_TEXT)
            textSize = 11f
            setTypeface(typeface, Typeface.BOLD)
            letterSpacing = 0.03f
            gravity = Gravity.CENTER
            val ic = TelegramIcon(dp(14f))
            setCompoundDrawablesRelative(ic, null, null, null)
            compoundDrawablePadding = dp(6f)
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(0xFFFF5B60.toInt(), 0xFFAE1220.toInt())).apply {
                cornerRadius = dp(16f).toFloat()
            }
            elevation = dp(2f).toFloat()
            setPadding(dp(11f), dp(6f), dp(13f), dp(6f))
            isClickable = true
            setOnClickListener { openChannel(); d.dismiss() }
        })
        card.addView(buttons, lp())

        // значок поверх верхней рамки карточки
        val frame = FrameLayout(act).apply { clipChildren = false; clipToPadding = false }
        frame.addView(card, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(21f)
        })
        frame.addView(Badge(act), FrameLayout.LayoutParams(dp(46f), dp(46f), Gravity.TOP or Gravity.CENTER_HORIZONTAL))

        // в альбомной ориентации окно может не влезть по высоте — прокрутка
        val scroll = ScrollView(act).apply {
            isFillViewport = true
            isVerticalScrollBarEnabled = false
            setPadding(dp(6f), dp(6f), dp(6f), dp(6f))
            clipToPadding = false
            addView(frame)
        }
        d.setContentView(scroll)
        d.window?.let { w ->
            w.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            val m = act.resources.displayMetrics
            val width = minOf((m.widthPixels * 0.86f).toInt(), dp(300f))
            w.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT)
            w.setDimAmount(0.75f)
        }
        d.show()
    }

    private fun lp(bottom: Int = 0) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = bottom }

    private fun box(act: Activity, color: Int, line: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = Ui.dp(act, 12f).toFloat()
        setStroke(Ui.dp(act, 1.2f), line)
    }

    // красный скруглённый квадрат с тёмно-синей каймой и синим свечением; внутри — маска паука (WebIcons)
    private class Badge(act: Activity) : View(act) {
        private val density = act.resources.displayMetrics.density
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val rect = RectF()

        init {
            setLayerType(LAYER_TYPE_SOFTWARE, null)  // свечение (setShadowLayer) — только в программной отрисовке
        }

        override fun onDraw(canvas: Canvas) {
            val pad = 6f * density
            rect.set(pad, pad, width - pad, height - pad)
            val rr = 15f * density
            // кайма (тёмная) со свечением
            p.shader = null
            p.style = Paint.Style.FILL
            p.color = 0xFF0B1633.toInt()
            p.setShadowLayer(6f * density, 0f, 0f, 0x993B6FD8.toInt())
            canvas.drawRoundRect(rect, rr, rr, p)
            p.clearShadowLayer()
            val inset = 2.5f * density
            rect.inset(inset, inset)
            p.shader = LinearGradient(rect.left, rect.top, rect.right, rect.bottom,
                0xFFE62429.toInt(), 0xFF8C101C.toInt(), Shader.TileMode.CLAMP)
            canvas.drawRoundRect(rect, rr - inset, rr - inset, p)
            p.shader = null
            // маска паука: синий круг, красная паутина, белые глаза
            val s = rect.width() * 0.0058f
            canvas.save()
            canvas.translate(rect.centerX() - 50f * s, rect.centerY() - 46f * s)
            canvas.scale(s, s)
            WebIcons.drawHead(canvas, 50f, 50f, 40f, 255, true)
            canvas.restore()
        }
    }

    // значок Telegram: тёмный круг с бумажным самолётиком цвета кнопки
    private class TelegramIcon(private val size: Int) : Drawable() {
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val plane = SvgPath.parse("M5.2 11.6 L17.6 6.8 C18.2 6.6 18.7 7 18.5 7.8 L16.4 17.6 C16.2 18.3 15.8 18.5 15.2 18.1 " +
            "L12 15.8 L10.5 17.3 C10.3 17.5 10.1 17.6 9.8 17.6 L10 14.3 L16 8.9 C16.3 8.6 15.9 8.5 15.5 8.7 L8.1 13.4 " +
            "L4.9 12.4 C4.2 12.2 4.2 11.8 5.2 11.6 Z")

        init {
            setBounds(0, 0, size, size)
        }

        override fun draw(canvas: Canvas) {
            val b = bounds
            val s = b.width() / 24f
            canvas.save()
            canvas.translate(b.left.toFloat(), b.top.toFloat())
            canvas.scale(s, s)
            p.color = 0xFFFFF3F4.toInt()
            canvas.drawCircle(12f, 12f, 12f, p)
            p.color = 0xFFC8161F.toInt()
            canvas.drawPath(plane, p)
            canvas.restore()
        }

        override fun getIntrinsicWidth() = size
        override fun getIntrinsicHeight() = size
        override fun setAlpha(alpha: Int) {
            p.alpha = alpha
        }

        override fun setColorFilter(colorFilter: ColorFilter?) {
            p.colorFilter = colorFilter
        }

        @Deprecated("Deprecated in Java")
        override fun getOpacity() = PixelFormat.TRANSLUCENT
    }
}
