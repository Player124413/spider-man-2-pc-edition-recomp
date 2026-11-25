package com.webheadnx.game

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

// Раскладка экранного управления: элементы и их положение (доли экрана — не зависит от разрешения).
// Файл filesDir/touch_layout.json правят и лаунчер (редактор), и игра (редактор поверх игры) — поэтому файл, а не
// SharedPreferences (у тех свой кеш в каждом процессе). Сохраняются только изменяемые поля по id: новые элементы
// из будущих версий появляются со значениями по умолчанию.
enum class Kind { BUTTON, STICK, DPAD }

class Control(
    val id: String,
    val kind: Kind,
    val label: String,
    var x: Float,              // центр: доля ширины экрана
    var y: Float,              // центр: доля высоты экрана
    var size: Float,           // диаметр: доля высоты экрана
    var visible: Boolean = true,
    var opacity: Float = 1f,   // множитель общей прозрачности
    val button: Int = -1,      // бит кнопки геймпада (SnxNative.*) или -1
    val vk: Int = 0,           // клавиша Windows (VK_*), если не кнопка
    val rightStick: Boolean = false,
    var floating: Boolean = false,  // стик появляется там, где коснулись (в своей половине экрана)
    var toggle: Boolean = false,    // кнопка залипает: нажал — держится до следующего нажатия
) {
    fun copy() = Control(id, kind, label, x, y, size, visible, opacity, button, vk, rightStick, floating, toggle)
}

object TouchLayout {
    private const val FILE = "touch_layout.json"
    // 2: стики по умолчанию статичные; 3/4: Minus («Пропуск») показывали и снова прятали;
    // 5: убрали кнопку зелий; 6: A/B шлют клавиши мыши/Ctrl, Minus виден для пропуска роликов;
    // 7: минимальный набор — только стик, удар, прыжок, паутина, пропуск, меню. Крестовина/ZL/правый стик скрыты.
    private const val VERSION = 7

    // Минимальный набор по экрану «Управление» в игре: ЛКМ — удар, Ctrl — прыжок, Enter — паутина/действие,
    // Space — пропуск ролика. Левый стик — движение, палец по пустому месту — камера. Всё остальное скрыто.
    fun defaults(): List<Control> = listOf(
        Control("lstick", Kind.STICK, "", 0.14f, 0.70f, 0.38f),  // статичный: не ездит под пальцем
        // A/B как клавиши: в Spider-Man 2 Joy2 через раз не давал двойной прыжок, а Fire надёжнее левой кнопки мыши.
        Control("a", Kind.BUTTON, L.t("Удар", "Attack"), 0.915f, 0.66f, 0.17f, vk = 0x01),
        Control("b", Kind.BUTTON, L.t("Прыжок", "Jump"), 0.825f, 0.83f, 0.17f, vk = 0x11),
        Control("zl", Kind.BUTTON, L.t("Зелья", "Potions"), 0.64f, 0.86f, 0.12f, visible = false, button = SnxNative.ZL),
        Control("plus", Kind.BUTTON, L.t("Меню", "Menu"), 0.58f, 0.07f, 0.10f, button = SnxNative.PLUS),
        // скрытые по умолчанию: включаются в редакторе
        Control("dpad", Kind.DPAD, "", 0.30f, 0.78f, 0.26f, visible = false),
        Control("rstick", Kind.STICK, "", 0.62f, 0.62f, 0.30f, visible = false, rightStick = true),
        Control("zr", Kind.BUTTON, "ZR", 0.70f, 0.13f, 0.11f, visible = false, button = SnxNative.ZR),
        Control("minus", Kind.BUTTON, L.t("Пропуск", "Skip"), 0.42f, 0.07f, 0.12f, visible = true, vk = 0x20),
        Control("rs", Kind.BUTTON, L.t("Центр", "Center"), 0.93f, 0.33f, 0.11f, visible = false, button = SnxNative.RSTICK),
        Control("esc", Kind.BUTTON, "Esc", 0.50f, 0.07f, 0.10f, visible = false, vk = 0x1B),
        Control("enter", Kind.BUTTON, L.t("Паутина", "Web"), 0.93f, 0.45f, 0.14f, visible = true, vk = 0x0D),
    ) + (1..9).map { n ->
        // зелья 1–9 — цифры клавиатуры (как в игре на ПК)
        Control("potion$n", Kind.BUTTON, "$n", 0.30f + 0.055f * (n - 1), 0.30f, 0.09f, visible = false, vk = 0x30 + n)
    }

    fun load(ctx: Context): MutableList<Control> {
        val list = defaults().map { it.copy() }.toMutableList()
        val f = File(ctx.filesDir, FILE)
        if (!f.exists()) return list
        try {
            val root = JSONObject(f.readText())
            val arr = root.optJSONArray("controls") ?: return list
            val ver = root.optInt("version", 1)
            val byId = list.associateBy { it.id }
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val c = byId[o.optString("id")] ?: continue
                c.x = o.optDouble("x", c.x.toDouble()).toFloat().coerceIn(0f, 1f)
                c.y = o.optDouble("y", c.y.toDouble()).toFloat().coerceIn(0f, 1f)
                c.size = o.optDouble("size", c.size.toDouble()).toFloat().coerceIn(MIN_SIZE, MAX_SIZE)
                if (ver >= 4 || c.id != "minus") c.visible = o.optBoolean("visible", c.visible)
                // обновляем раскладку без потери пользовательских позиций: зелья убраны, пропуск добавлен
                // минимальный набор: лишние элементы из старых раскладок не показываем
                if (c.id == "zl" || c.id == "dpad" || c.id == "rstick" || c.id.startsWith("potion")) c.visible = false
                if (c.id == "minus" || c.id == "enter") c.visible = true
                c.opacity = o.optDouble("opacity", c.opacity.toDouble()).toFloat().coerceIn(0.1f, 1f)
                if (ver >= 2) c.floating = o.optBoolean("floating", c.floating)
                c.toggle = o.optBoolean("toggle", c.toggle)
            }
        } catch (e: Exception) {
            // повреждённый файл — раскладка по умолчанию
        }
        return list
    }

    fun save(ctx: Context, list: List<Control>) {
        val arr = JSONArray()
        for (c in list) {
            arr.put(JSONObject().apply {
                put("id", c.id)
                put("x", c.x.toDouble())
                put("y", c.y.toDouble())
                put("size", c.size.toDouble())
                put("visible", c.visible)
                put("opacity", c.opacity.toDouble())
                put("floating", c.floating)
                put("toggle", c.toggle)
            })
        }
        val root = JSONObject().put("version", VERSION).put("controls", arr)
        val f = File(ctx.filesDir, FILE)
        val tmp = File(ctx.filesDir, "$FILE.tmp")
        tmp.writeText(root.toString(1))
        tmp.renameTo(f)
    }

    fun reset(ctx: Context) {
        File(ctx.filesDir, FILE).delete()
    }

    const val MIN_SIZE = 0.06f
    const val MAX_SIZE = 0.55f
}
