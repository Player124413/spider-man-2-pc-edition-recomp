package com.webheadnx.game

import java.util.Locale

// Язык интерфейса — по языку телефона: русский (и белорусский, казахский — у них обычно русский вторым), иначе
// английский. Строки — парами прямо в коде (L.t("рус", "eng")): интерфейс собран кодом (Ui.kt), а не разметкой.
object L {
    val ru: Boolean
        get() = Locale.getDefault().language in RU_LANGS

    fun t(ru: String, en: String): String = if (this.ru) ru else en

    private val RU_LANGS = setOf("ru", "be", "kk")

    // автор порта и благодарность (лаунчер: окно при первом запуске и кнопка «О порте»)
    const val AUTHOR = "Player1444"
    const val TG_CHANNEL = "https://t.me/player1444ports"
    const val UPSTREAM = "https://github.com/Player124413/Shrek2-android_edition"
    const val DESIGNER = "@base64E"  // редизайн окна «О порте» (CreditsDialog)
}
