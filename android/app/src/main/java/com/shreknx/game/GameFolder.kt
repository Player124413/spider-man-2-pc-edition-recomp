package com.shreknx.game

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File

// Папка игры: проверка содержимого, путь из выбора папки системным диалогом (SAF) и копирование оттуда в
// каталог приложения («Импорт») — для копирования разрешение на все файлы не нужно.
object GameFolder {
    // имя без учёта регистра (игра с ПК, раздел телефона различает регистр; vfs.c ищет так же)
    fun child(dir: File, name: String): File? =
        dir.listFiles()?.firstOrNull { it.name.equals(name, ignoreCase = true) }

    // null — всё на месте, иначе — чего не хватает
    fun check(path: String): String? {
        val dir = File(path)
        if (!dir.isDirectory) return L.t("Папка не найдена: $path", "Folder not found: $path")
        if (dir.listFiles() == null) return L.t("Нет доступа к папке — используй «Импорт»", "No access to the folder — use “Import”")
        val sys = child(dir, "System") ?: return L.t("В папке нет System — нужна папка установленной игры с ПК", "No System folder — select the folder of the game installed on a PC")
        for (f in listOf("Core.u", "Engine.u")) if (child(sys, f) == null) return L.t("В System нет $f", "System has no $f")
        if (child(dir, "Maps") == null) return L.t("В папке нет Maps", "No Maps folder")
        return null
    }

    // Модули игры (PE: их грузит runtime/loader.c, файл должен совпадать байт в байт). Пропали из System (на ZTE
    // Blade 20 Smart Webhead.exe «через некоторое время» исчез: очистка памяти или антивирус удаляют .exe с ПК) и
    // нет копии во внутренней памяти (Settings.moduleDir, её делает загрузчик после удачного запуска) —
    // список пропавших, иначе пусто.
    val MODULES = listOf("Webhead.exe", "Core.dll", "Engine.dll", "WinDrv.dll", "D3DDrv.dll")

    fun missingModules(gameDir: String, moduleDir: File): List<String> {
        val sys = child(File(gameDir), "System") ?: return emptyList()  // нет System — скажет check()
        return MODULES.filter { child(sys, it)?.takeIf { f -> f.length() > 0 } == null && child(moduleDir, it) == null }
    }

    // модули, которых нет в System, но есть копия (игра запустится с копией)
    fun fromBackup(gameDir: String, moduleDir: File): List<String> {
        val sys = child(File(gameDir), "System") ?: return emptyList()
        return MODULES.filter { child(sys, it) == null && child(moduleDir, it) != null }
    }

    fun missingText(missing: List<String>): String = L.t(
        "В папке игры пропали файлы: System/${missing.joinToString(", ")}.\n\n" +
            "Чаще всего их удаляет «Очистка» / «Диспетчер телефона» или антивирус: файлы .exe и .dll с ПК они " +
            "считают мусором или вирусом. Скопируйте их заново — «Импорт из папки…» (та же копия игры) или по USB в " +
            "System. После следующего удачного запуска порт хранит копию этих файлов во внутренней памяти " +
            "приложения, куда очистка и антивирусы не добираются, — и игра запустится, даже если их снова удалят.",
        "Files are missing from the game folder: System/${missing.joinToString(", ")}.\n\n" +
            "Usually a phone cleaner or an antivirus deletes them: .exe and .dll files from a PC look like junk or " +
            "malware to them. Copy them again — “Import from folder…” (the same game copy) or over USB into " +
            "System. After the next successful start the port keeps a copy of these files in the app's internal " +
            "storage, out of reach of cleaners and antiviruses, so the game starts even if they are deleted again.")

    // Архивы, скачанные с GitHub без Git LFS, содержат вместо .bik текстовые указатели по ~132 байта.
    // Игра при этом запускается, но все ролики (Activision, Marvel, вступления) отсутствуют.
    fun movieProblem(path: String): String? {
        val movies = child(File(path), "Movies")
            ?: return L.t("Нет папки Movies — ролики не покажутся", "No Movies folder — videos will not play")
        val biks = movies.listFiles()?.filter { it.isFile && it.name.endsWith(".bik", true) }.orEmpty()
        if (biks.isEmpty())
            return L.t("В Movies нет .bik — ролики не покажутся", "No .bik files in Movies — videos will not play")
        val lfs = biks.firstOrNull { f ->
            if (f.length() >= 4096) false else try {
                f.bufferedReader().use { it.readLine()?.startsWith("version https://git-lfs.github.com/spec/v1") == true }
            } catch (e: Exception) { false }
        }
        if (lfs != null)
            return L.t("${lfs.name} — указатель Git LFS (${lfs.length()} байт), не видео. Возьмите реальные .bik " +
                "(git lfs pull в клоне игры) и скопируйте в Movies",
                "${lfs.name} is a Git LFS pointer (${lfs.length()} bytes), not a video. Get real .bik files " +
                "(git lfs pull in the game clone) and copy them into Movies")
        return null
    }

    class Progress(var files: Int = 0, var bytes: Long = 0, var current: String = "")

    // рекурсивное копирование дерева SAF в dst; cancel() — прервать; onProgress зовётся из потока копирования
    fun import(cr: ContentResolver, tree: Uri, dst: File, cancel: () -> Boolean, onProgress: (Progress) -> Unit) {
        val p = Progress()
        val root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        copyDir(cr, tree, DocumentsContract.getDocumentId(root), dst, p, cancel, onProgress)
    }

    private fun copyDir(cr: ContentResolver, tree: Uri, docId: String, dst: File, p: Progress,
                        cancel: () -> Boolean, onProgress: (Progress) -> Unit) {
        dst.mkdirs()
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        val cols = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                           DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE)
        val entries = ArrayList<Triple<String, String, Boolean>>()
        cr.query(children, cols, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                entries += Triple(c.getString(0), c.getString(1), c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR)
            }
        }
        for ((id, name, isDir) in entries) {
            if (cancel()) return
            val out = File(dst, name)
            if (isDir) {
                copyDir(cr, tree, id, out, p, cancel, onProgress)
                continue
            }
            p.current = name
            onProgress(p)
            val uri = DocumentsContract.buildDocumentUriUsingTree(tree, id)
            val tmp = File(dst, "$name.part")
            cr.openInputStream(uri)?.use { input ->
                tmp.outputStream().use { o ->
                    val buf = ByteArray(1 shl 20)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        o.write(buf, 0, n)
                        p.bytes += n
                        if (cancel()) return
                    }
                }
            }
            tmp.renameTo(out)
            p.files++
        }
    }
}
