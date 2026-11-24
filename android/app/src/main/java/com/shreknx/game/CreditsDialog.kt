package com.shreknx.game

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
import android.text.TextPaint
import android.text.method.LinkMovementMethod
import android.text.style.ClickableSpan
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

// Окно «О порте» (первый запуск, обновление, кнопка «О порте»): тёмно-зелёная карточка с тонкой зелёной рамкой,
// сверху на рамке — зелёный значок с головой огра; заголовок, автор порта, Telegram-канал, благодарность
// репозиторию webheadNX и автору дизайна окна (@base64E), кнопки «OK» / «Открыть канал». Строки — по языку телефона (L.t).
object CreditsDialog {
    private const val CARD_BG = 0xFF111A0E.toInt()
    private const val CARD_LINE = 0xFF34501C.toInt()
    private const val BOX_GREEN = 0xFF1C2C12.toInt()
    private const val BOX_GREEN_LINE = 0xFF3D5F1F.toInt()
    private const val BOX_DARK = 0xFF172117.toInt()
    private const val BOX_DARK_LINE = 0xFF233022.toInt()
    private const val GREY = 0xFF9AA595.toInt()
    private const val GREEN = 0xFF9BCB3C.toInt()
    private const val BTN_TEXT = 0xFF0E1508.toInt()

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
                cornerRadius = dp(22f).toFloat()
                setStroke(dp(1.5f), CARD_LINE)
            }
            setPadding(dp(16f), dp(36f), dp(16f), dp(8f))
        }
        // заголовок: «Spider-Man 2» белым жирным, « · webheadNX Android» зелёным
        card.addView(TextView(act).apply {
            val sb = SpannableStringBuilder()
            sb.append("Spider-Man 2", StyleSpan(Typeface.BOLD), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            val st = sb.length
            sb.append(" · webheadNX Android")
            sb.setSpan(ForegroundColorSpan(GREEN), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text = sb
            setTextColor(Color.WHITE)
            textSize = 18f
            gravity = Gravity.CENTER
        }, lp(bottom = dp(10f)))

        // автор
        card.addView(TextView(act).apply {
            val sb = SpannableStringBuilder(L.t("Android-порт сделан ", "Android port by "))
            val st = sb.length
            sb.append(L.AUTHOR)
            sb.setSpan(StyleSpan(Typeface.BOLD), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.setSpan(ForegroundColorSpan(GREEN), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text = sb
            setTextColor(Color.WHITE)
            textSize = 14f
            gravity = Gravity.CENTER
            background = box(act, BOX_GREEN, BOX_GREEN_LINE)
            setPadding(dp(10f), dp(8f), dp(10f), dp(8f))
        }, lp(bottom = dp(8f)))

        // Telegram-канал: 📢, подпись капсом, ссылка; нажатие — открыть канал
        val tg = LinearLayout(act).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = box(act, BOX_DARK, BOX_DARK_LINE)
            setPadding(dp(12f), dp(7f), dp(12f), dp(7f))
            isClickable = true
            setOnClickListener { openChannel() }
        }
        tg.addView(TextView(act).apply { text = "📢"; textSize = 20f }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(10f) })
        val tgText = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        tgText.addView(TextView(act).apply {
            text = L.t("TELEGRAM-КАНАЛ", "TELEGRAM CHANNEL")
            setTextColor(GREY)
            textSize = 10f
            letterSpacing = 0.08f
        })
        tgText.addView(TextView(act).apply {
            text = L.TG_CHANNEL.removePrefix("https://")
            setTextColor(GREEN)
            textSize = 14f
            setTypeface(typeface, Typeface.BOLD)
        })
        tg.addView(tgText)
        card.addView(tg, lp(bottom = dp(8f)))

        // благодарность: ссылка на репозиторий — зелёная жирная, нажимается
        card.addView(TextView(act).apply {
            val link = L.UPSTREAM.removePrefix("https://")
            val sb = SpannableStringBuilder(L.t("Огромное спасибо репозиторию ", "Huge thanks to the "))
            val st = sb.length
            sb.append(link)
            sb.setSpan(object : ClickableSpan() {
                override fun onClick(widget: View) {
                    try {
                        act.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(L.UPSTREAM)))
                    } catch (e: Exception) {
                    }
                }

                override fun updateDrawState(ds: TextPaint) {
                    ds.color = GREEN
                    ds.isFakeBoldText = true
                    ds.isUnderlineText = false
                }
            }, st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.append(L.t(" — без него Android-порта бы не существовало.",
                " repository — without it the Android port would not exist."))
            text = sb
            setTextColor(GREY)
            textSize = 12.5f
            setLineSpacing(0f, 1.1f)
            movementMethod = LinkMovementMethod.getInstance()
            highlightColor = Color.TRANSPARENT
            setPadding(dp(4f), 0, dp(4f), 0)
        }, lp(bottom = dp(8f)))

        // благодарность за дизайн окна: ник — зелёный жирный
        card.addView(TextView(act).apply {
            val sb = SpannableStringBuilder(L.t("Спасибо ", "Thanks to "))
            val st = sb.length
            sb.append(L.DESIGNER)
            sb.setSpan(StyleSpan(Typeface.BOLD), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.setSpan(ForegroundColorSpan(GREEN), st, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sb.append(L.t(" за редизайн всплывающего окна.", " for the redesign of this pop-up window."))
            text = sb
            setTextColor(GREY)
            textSize = 12.5f
            setPadding(dp(4f), 0, dp(4f), 0)
        }, lp(bottom = dp(8f)))

        // разделитель
        card.addView(View(act).apply { setBackgroundColor(BOX_DARK_LINE) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1f)).apply { bottomMargin = dp(4f) })

        // кнопки: «OK» — текстом, «Открыть канал» — зелёная «таблетка» со значком Telegram
        val buttons = LinearLayout(act).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL or Gravity.END
        }
        buttons.addView(TextView(act).apply {
            text = "OK"
            setTextColor(0xFFB8C2B0.toInt())
            textSize = 13f
            setTypeface(typeface, Typeface.BOLD)
            gravity = Gravity.CENTER
            setPadding(dp(16f), dp(8f), dp(16f), dp(8f))
            isClickable = true
            setOnClickListener { d.dismiss() }
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(8f) })
        buttons.addView(TextView(act).apply {
            text = L.t("ОТКРЫТЬ КАНАЛ", "OPEN CHANNEL")
            setTextColor(BTN_TEXT)
            textSize = 12f
            setTypeface(typeface, Typeface.BOLD)
            letterSpacing = 0.03f
            gravity = Gravity.CENTER
            val ic = TelegramIcon(dp(16f))
            setCompoundDrawablesRelative(ic, null, null, null)
            compoundDrawablePadding = dp(7f)
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(0xFFA6D650.toInt(), 0xFF6E9E26.toInt())).apply {
                cornerRadius = dp(20f).toFloat()
            }
            elevation = dp(4f).toFloat()
            setPadding(dp(14f), dp(8f), dp(16f), dp(8f))
            isClickable = true
            setOnClickListener { openChannel(); d.dismiss() }
        })
        card.addView(buttons, lp())

        // значок поверх верхней рамки карточки
        val frame = FrameLayout(act).apply { clipChildren = false; clipToPadding = false }
        frame.addView(card, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(28f)
        })
        frame.addView(Badge(act), FrameLayout.LayoutParams(dp(62f), dp(62f), Gravity.TOP or Gravity.CENTER_HORIZONTAL))

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
            val width = minOf((m.widthPixels * 0.9f).toInt(), dp(340f))
            w.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT)
            w.setDimAmount(0.75f)
        }
        d.show()
    }

    private fun lp(bottom: Int = 0) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = bottom }

    private fun box(act: Activity, color: Int, line: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = Ui.dp(act, 16f).toFloat()
        setStroke(Ui.dp(act, 1.2f), line)
    }

    // зелёный скруглённый квадрат с тёмной каймой и свечением; внутри — тёмный силуэт головы огра с ушами
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
            p.color = 0xFF0B1007.toInt()
            p.setShadowLayer(6f * density, 0f, 0f, 0x998DC63F.toInt())
            canvas.drawRoundRect(rect, rr, rr, p)
            p.clearShadowLayer()
            val inset = 2.5f * density
            rect.inset(inset, inset)
            p.shader = LinearGradient(rect.left, rect.top, rect.right, rect.bottom, 0xFFA6D650.toInt(), 0xFF5E8A22.toInt(), Shader.TileMode.CLAMP)
            canvas.drawRoundRect(rect, rr - inset, rr - inset, p)
            p.shader = null
            // голова огра силуэтом: уши и круг — тёмные, лицо — вырезом цвета значка
            val s = rect.width() * 0.0058f
            canvas.save()
            canvas.translate(rect.centerX() - 50f * s, rect.centerY() - 46f * s)
            canvas.scale(s, s)
            val ink = 0xFF0E1508.toInt()
            ShrekIcons.drawEars(canvas, 52f, ink, ink, ink)
            p.color = ink
            canvas.drawCircle(50f, 50f, 40f, p)
            p.color = 0xFF86BC38.toInt()
            canvas.drawCircle(38f, 48f, 6f, p)
            canvas.drawCircle(62f, 48f, 6f, p)
            p.style = Paint.Style.STROKE
            p.strokeWidth = 5.5f
            p.strokeCap = Paint.Cap.ROUND
            canvas.drawArc(RectF(33f, 52f, 67f, 74f), 20f, 140f, false, p)
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
            p.color = BTN_TEXT
            canvas.drawCircle(12f, 12f, 12f, p)
            p.color = 0xFF94C843.toInt()
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
