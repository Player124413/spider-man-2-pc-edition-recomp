package com.webheadnx.game

import android.app.Activity
import android.app.AlertDialog
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File

// Выбор уровня: карты из папки игры (Maps/*.unr) — сюжетные (имя с номера: 1_Shreks_Swamp), бонусные
// (Beanstalk_bonus, _dawn, _knight — мини-игра с монетами на ферме Джека), «книги» (Book_*: меню-книга, сказка) и
// остальные. Игра запускается сразу на карте (командная строка после «--», host/boot/main.c) — минуя сюжет:
// инвентарь и прогресс — как с начала уровня.
// Бонусные карты напрямую раньше теряли героя (камера у ворот, Шрек невидим, таймер стоит): скрипт игры
// Shrek.PostLoadGame снимал с героя метку MainPlayer и возвращал её, только если герой пришёл с другого уровня
// (в игре — с плаката «Разыскивается»). Порт исправляет байткод в памяти при загрузке (runtime/script_fix.c).
// Время мини-игры без плакатов — самое долгое (CoinBonusGame.GameTimeLimits[3]); по концу времени или падению
// LeaveBonusLevelTrigger загружает слот GameSlot (последний выбранный в меню игры) — монеты добавятся к нему; без
// него уровень не закрывается — выход через меню паузы.
object Levels {
    class Group(val title: String, val maps: List<String>)

    fun maps(gameDir: String): List<String> {
        val dir = GameFolder.child(File(gameDir), "Maps") ?: return emptyList()
        return dir.listFiles()?.filter { it.isFile && it.name.endsWith(".unr", ignoreCase = true) }?.map { it.name } ?: emptyList()
    }

    private fun num(name: String): Int = Regex("^(\\d+)_").find(name)?.groupValues?.get(1)?.toIntOrNull() ?: Int.MAX_VALUE

    fun groups(maps: List<String>): List<Group> {
        val story = maps.filter { num(it) != Int.MAX_VALUE }.sortedWith(compareBy({ num(it) }, { it.lowercase() }))
        val rest = maps - story.toSet()
        val bonus = rest.filter { isBonus(it) }.sortedBy { it.lowercase() }
        val books = (rest - bonus.toSet()).filter { it.startsWith("Book", ignoreCase = true) }.sortedBy { it.lowercase() }
        val other = (rest - bonus.toSet() - books.toSet()).sortedBy { it.lowercase() }
        return listOf(
            Group(L.t("Сюжет", "Story"), story),
            Group(L.t("Бонусные уровни", "Bonus levels"), bonus),
            Group(L.t("Книги", "Books"), books),
            Group(L.t("Другие карты", "Other maps"), other),
        ).filter { it.maps.isNotEmpty() }
    }

    fun isBonus(map: String) = map.contains("bonus", ignoreCase = true) || map.startsWith("Beanstalk", ignoreCase = true)

    fun title(map: String) = map.substringBeforeLast('.').replace('_', ' ')

    // список по группам; onPick — имя файла карты
    fun show(a: Activity, gameDir: String, onPick: (String) -> Unit) {
        val all = maps(gameDir)
        if (all.isEmpty()) {
            AlertDialog.Builder(a).setMessage(L.t("В папке игры нет карт (Maps/*.unr).", "The game folder has no maps (Maps/*.unr)."))
                .setPositiveButton("OK", null).show()
            return
        }
        val box = Ui.vbox(a, 16)
        box.addView(Ui.label(a, L.t("Уровень запускается напрямую, минуя сюжет: инвентарь и прогресс — как с нуля. " +
            "Не сохраняйтесь поверх нужного слота.",
            "The level starts directly, skipping the story: inventory and progress start from scratch. " +
            "Don't save over a slot you need."), dim = true, size = 12f))
        if (all.any { isBonus(it) })
            box.addView(Ui.label(a, L.t("Бонусные уровни (ферма Джека): вступление рассказчика, затем мини-игра с " +
                "монетами на время. Когда время выйдет или герой упадёт, игра загрузит слот, последним выбранный " +
                "в меню игры (собранные монеты добавятся к нему, как после плаката); если сохранений нет — выход " +
                "через меню паузы.",
                "Bonus levels (Jack's farm): the narrator's intro, then the timed coin mini-game. When time runs out " +
                "or the hero falls, the game loads the save slot last chosen in the game menu (the coins you " +
                "collected are added to it, as after a wanted poster); with no saves, leave through the pause menu."),
                dim = true, size = 12f).apply { setPadding(0, Ui.dp(a, 6f), 0, 0) })
        var dlg: AlertDialog? = null
        for (g in groups(all)) {
            box.addView(Ui.title(a, g.title, 16f).apply { setPadding(0, Ui.dp(a, 12f), 0, Ui.dp(a, 4f)) })
            for (m in g.maps) {
                box.addView(TextView(a).apply {
                    text = title(m)
                    textSize = 15f
                    setTextColor(Ui.TEXT)
                    gravity = Gravity.CENTER_VERTICAL
                    val p = Ui.dp(a, 10f)
                    setPadding(p, p, p, p)
                    background = Ui.rounded(0xFF2B3720.toInt(), Ui.dp(a, 10f).toFloat())
                    isFocusable = true  // выбор крестовиной геймпада
                    setOnClickListener { dlg?.dismiss(); onPick(m) }
                }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                    .apply { bottomMargin = Ui.dp(a, 6f) })
            }
        }
        dlg = AlertDialog.Builder(a).setTitle(L.t("Выбор уровня", "Level select"))
            .setView(ScrollView(a).apply { addView(box) })
            .setNegativeButton(L.t("Отмена", "Cancel"), null).show()
    }
}
