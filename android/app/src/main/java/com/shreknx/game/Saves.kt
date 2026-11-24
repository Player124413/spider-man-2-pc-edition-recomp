package com.shreknx.game

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// Импорт сохранений (например, с ПК): папка выбирается системным диалогом (SAF), файлы копируются туда, где их
// ищет игра в порте. Путь — SavePath из [Core.System] game.ini (или Default.ini игры), относительно System;
// запись игры идёт в каталог пользователя (оверлей VFS), туда и кладём: <user>/<SavePath>, обычно user/Save.
// Источник: сама папка Save, папка с подпапкой Save (например, «Spider-Man 2» целиком), иначе — файлы Save*/*.usa
// из выбранной папки. Прежние сохранения не теряются: переименовываются в Save.bak-<дата>.
object Saves {
    class Result(val files: Int, val target: File, val backup: File?)

    // каталог сохранений игры в порте
    fun dir(ctx: Context, s: Settings): File {
        val user = Settings.userDir(ctx)
        val sp = savePath(File(user, "System/game.ini"))
            ?: GameFolder.child(File(s.gameDir), "System")?.let { sys -> GameFolder.child(sys, "Default.ini")?.let { savePath(it) } }
            ?: "..\\Save"
        val rel = sp.replace('\\', '/').trim()
        if (rel.isEmpty() || rel.startsWith("/") || Regex("^[A-Za-z]:").containsMatchIn(rel)) return File(user, "Save")
        // по частям, без учёта регистра (каталог мог создать сам игра с другим регистром — vfs.c ищет так же)
        var cur = File(user, "System")
        for (part in rel.split('/')) {
            cur = when (part) {
                "", "." -> cur
                ".." -> cur.parentFile ?: cur
                else -> GameFolder.child(cur, part) ?: File(cur, part)
            }
        }
        val c = cur.canonicalFile
        return if (c.path.startsWith(user.canonicalPath + File.separator)) c else File(user, "Save")
    }

    // SavePath из [Core.System] ini (8 бит или UTF-16LE с BOM, как пишет игра)
    fun savePath(ini: File): String? {
        if (!ini.isFile) return null
        val b = try { ini.readBytes() } catch (e: Exception) { return null }
        val text = if (b.size >= 2 && b[0] == 0xFF.toByte() && b[1] == 0xFE.toByte()) String(b, 2, b.size - 2, Charsets.UTF_16LE)
            else String(b, Charsets.ISO_8859_1)
        var inSec = false
        for (raw in text.lines()) {
            val l = raw.trim()
            if (l.startsWith("[")) { inSec = l.equals("[Core.System]", ignoreCase = true); continue }
            if (inSec && l.startsWith("SavePath=", ignoreCase = true)) return l.substring(9).trim().ifEmpty { null }
        }
        return null
    }

    private class Doc(val id: String, val name: String, val dir: Boolean)

    private fun children(cr: ContentResolver, tree: Uri, docId: String): List<Doc> {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        val cols = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                           DocumentsContract.Document.COLUMN_MIME_TYPE)
        val out = ArrayList<Doc>()
        cr.query(uri, cols, null, null, null)?.use { c ->
            while (c.moveToNext()) out += Doc(c.getString(0), c.getString(1) ?: "", c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR)
        }
        return out
    }

    // папка Save в дереве (до 3 уровней вглубь)
    private fun findSave(cr: ContentResolver, tree: Uri, docId: String, depth: Int): String? {
        val kids = children(cr, tree, docId)
        kids.firstOrNull { it.dir && it.name.equals("Save", ignoreCase = true) }?.let { return it.id }
        if (depth <= 0) return null
        for (k in kids) if (k.dir) findSave(cr, tree, k.id, depth - 1)?.let { return it }
        return null
    }

    private fun isSaveFile(name: String) = name.startsWith("save", ignoreCase = true) || name.endsWith(".usa", ignoreCase = true)

    private fun copy(cr: ContentResolver, tree: Uri, id: String, out: File) {
        val tmp = File(out.parentFile, out.name + ".part")
        cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id))?.use { i -> tmp.outputStream().use { i.copyTo(it, 1 shl 16) } }
            ?: throw IllegalStateException(L.t("не открыть ${out.name}", "cannot open ${out.name}"))
        if (out.exists()) out.delete()
        if (!tmp.renameTo(out)) throw IllegalStateException(L.t("не записать ${out.name}", "cannot write ${out.name}"))
    }

    private fun copyTree(cr: ContentResolver, tree: Uri, docId: String, dst: File): Int {
        dst.mkdirs()
        var n = 0
        for (k in children(cr, tree, docId)) {
            if (k.dir) n += copyTree(cr, tree, k.id, File(dst, k.name))
            else { copy(cr, tree, k.id, File(dst, k.name)); n++ }
        }
        return n
    }

    fun import(ctx: Context, s: Settings, tree: Uri): Result {
        val cr = ctx.contentResolver
        val rootId = DocumentsContract.getTreeDocumentId(tree)
        val rootName = cr.query(DocumentsContract.buildDocumentUriUsingTree(tree, rootId),
            arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { if (it.moveToFirst()) it.getString(0) else null } ?: ""
        val saveId = if (rootName.equals("Save", ignoreCase = true)) rootId else findSave(cr, tree, rootId, 3)
        val loose = if (saveId == null) children(cr, tree, rootId).filter { !it.dir && isSaveFile(it.name) } else emptyList()
        if (saveId == null && loose.isEmpty())
            throw IllegalStateException(L.t("Не нашёл сохранений: выбери папку Save (или папку, где она лежит).",
                "No saves found: pick the Save folder (or the folder that contains it)."))
        val target = dir(ctx, s)
        var backup: File? = null
        if (target.isDirectory && (target.list()?.isNotEmpty() == true)) {
            val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date())
            backup = File(target.parentFile, "${target.name}.bak-$stamp")
            if (!target.renameTo(backup)) throw IllegalStateException(L.t("Не удалось сохранить копию прежних сохранений", "Could not back up the current saves"))
        }
        target.mkdirs()
        val n = if (saveId != null) copyTree(cr, tree, saveId, target)
            else { loose.forEach { copy(cr, tree, it.id, File(target, it.name)) }; loose.size }
        return Result(n, target, backup)
    }
}
