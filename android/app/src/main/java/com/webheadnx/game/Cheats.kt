package com.webheadnx.game

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.os.Process

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

    // Выбор уровня из читов: игра в процессе :game, поэтому сначала уходим в лаунчер, а он перезапускает
    // игру уже на выбранной карте (LauncherActivity.EXTRA_LEVEL → gameArgs(map)).
    private fun level(a: Activity, map: String) {
        a.startActivity(Intent(a, LauncherActivity::class.java)
            .putExtra(LauncherActivity.EXTRA_LEVEL, map)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP))
        Handler(Looper.getMainLooper()).postDelayed({
            try { SnxNative.exitNow() } catch (e: UnsatisfiedLinkError) {}
            Process.killProcess(Process.myPid())
        }, 400)
    }

    fun show(a: Activity, onDismiss: () -> Unit) {
        val items = listOf(L.t("Выбор уровня…", "Level select…")) + LIST.map { L.t(it.ru, it.en) }
        AlertDialog.Builder(a)
            .setTitle(L.t("Читы", "Cheats"))
            .setItems(items.toTypedArray()) { _, i ->
                if (i == 0) Levels.show(a, Settings(a).gameDir) { map -> level(a, map) }
                else send(LIST[i - 1].vk)
            }
            .setNegativeButton(L.t("Закрыть", "Close"), null)
            .setOnDismissListener { onDismiss() }
            .show()
    }
}
