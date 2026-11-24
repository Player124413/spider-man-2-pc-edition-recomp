package com.webheadnx.game

import android.app.Activity
import android.app.AlertDialog
import android.os.Handler
import android.os.Looper

// Читы поверх игры: команды игры привязаны к клавишам цифрового блока (host/boot/config.c, g_cheat_bindings —
// тот же список, SNX_CHEATS=1 от лаунчера), меню нажимает клавишу. Открывается шестерёнкой (при включённых
// читах) или с геймпада — нажатием обоих стиков (L3+R3). Режим и меню разработчика убраны (по просьбе автора).
object Cheats {
    class Cheat(val vk: Int, val ru: String, val en: String)

    val LIST = listOf(
        Cheat(0x62, "Сквозь стены (noclip)", "Through walls (noclip)"),
        Cheat(0x63, "Полёт", "Fly"),
        Cheat(0x64, "Обычная ходьба (выключить полёт и noclip)", "Walk normally (turn off fly and noclip)"),
        Cheat(0x65, "Бессмертие", "God mode"),
        Cheat(0x66, "+100 монет", "+100 coins"),
        Cheat(0x67, "Зелья", "Potions"),
        Cheat(0x68, "Все плакаты «Разыскивается» (бонусы)", "All wanted posters (bonuses)"),
        Cheat(0x69, "Интерфейс вкл/выкл", "HUD on/off"),
    )

    // нажать клавишу чуть позже: окно меню закрылось, фокус вернулся к игре
    fun send(vk: Int) {
        val h = Handler(Looper.getMainLooper())
        h.postDelayed({ try { SnxNative.key(vk, true) } catch (e: UnsatisfiedLinkError) {} }, 250)
        h.postDelayed({ try { SnxNative.key(vk, false) } catch (e: UnsatisfiedLinkError) {} }, 350)
    }

    fun show(a: Activity, onDismiss: () -> Unit) {
        AlertDialog.Builder(a)
            .setTitle(L.t("Читы", "Cheats"))
            .setItems(LIST.map { L.t(it.ru, it.en) }.toTypedArray()) { _, i -> send(LIST[i].vk) }
            .setNegativeButton(L.t("Закрыть", "Close"), null)
            .setOnDismissListener { onDismiss() }
            .show()
    }
}
