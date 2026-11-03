package at.raumusik.xcamvr

import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadataRetriever
import android.media.MediaScannerConnection
import android.os.Environment
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/** Ein Video in der Übersicht (Name aus der Foto-App übernommen). */
data class StoredPhoto(
    val id: String,
    val file: File,
    val thumb: File,
    val filename: String,
    val cameraMake: String,
    val cameraModel: String,
    val width: Int,
    val height: Int,
    val createTime: String,
    val durationMs: Long = 0
) {
    val isXcam: Boolean
        get() = listOf(cameraMake, cameraModel, filename).any {
            it.contains("slam", true) || it.contains("xcam", true)
        }
    val cameraLabel: String
        get() = listOf(cameraMake, cameraModel).filter { it.isNotBlank() }.distinct().joinToString(" ")
}

/**
 * Die Videos liegen genau einmal im öffentlichen Ordner „Movies/XCAM“
 * (sichtbar für andere Player, die Dateien-App und per USB, bleibt bei Deinstallation erhalten).
 * Im privaten App-Speicher liegen nur Vorschaubilder und Infodateien.
 * Auch Videos, die man selbst (z. B. per USB) in diesen Ordner kopiert, werden angezeigt.
 */
class PhotoStore(private val ctx: Context) {
    val publicDir: File = File(
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES), "XCAM"
    )
    private val privDir = File(ctx.filesDir, "videos").apply { mkdirs() }
    private val videoExt = setOf("mp4", "mov", "mkv", "webm", "m4v", "3gp")

    private fun safe(id: String) = id.replace(Regex("[^A-Za-z0-9_-]"), "_").take(80)

    fun thumbFor(id: String) = File(privDir, safe(id) + ".thumb.jpg")
    private fun metaFor(id: String) = File(privDir, safe(id) + ".json")

    private fun readMeta(id: String): JSONObject? =
        metaFor(id).takeIf { it.exists() }?.let { try { JSONObject(it.readText()) } catch (_: Exception) { null } }

    fun exists(id: String): Boolean {
        val m = readMeta(id) ?: return false
        return File(publicDir, m.optString("file")).exists()
    }

    /** Zieldatei: bereits begonnener Download desselben Videos wird fortgesetzt. */
    fun targetFor(id: String, filename: String): File {
        publicDir.mkdirs()
        readMeta(id)?.optString("pending")?.takeIf { it.isNotBlank() }?.let { return File(publicDir, it) }
        var name = filename.replace(Regex("[\\\\/:*?\"<>|]"), "_").ifBlank { "video.mp4" }
        if (name.substringAfterLast('.', "").lowercase() !in videoExt) name += ".mp4"
        val base = name.substringBeforeLast('.')
        val ext = name.substringAfterLast('.')
        var f = File(publicDir, name)
        var n = 2
        while (f.exists() || partFor(f).exists()) { f = File(publicDir, "$base ($n).$ext"); n++ }
        metaFor(id).writeText(JSONObject().put("id", id).put("pending", f.name).toString())
        return f
    }

    /** Teil-Datei während des Downloads (versteckt, gleiche Endung, damit Android sie akzeptiert). */
    fun partFor(target: File) = File(target.parentFile, ".laden-" + target.name)

    fun saveMeta(id: String, file: File, meta: JSONObject) {
        val info = probe(file)
        meta.put("id", id).put("file", file.name)
            .put("width", info.first).put("height", info.second).put("durationMs", info.third)
        metaFor(id).writeText(meta.toString())
        MediaScannerConnection.scanFile(ctx, arrayOf(file.absolutePath), null, null)
    }

    private val isoFormat = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply {
        timeZone = TimeZone.getTimeZone("UTC")
    }

    private val probeCache = HashMap<String, Triple<Int, Int, Long>>()

    /** Breite, Höhe, Dauer eines Videos (zwischengespeichert). */
    fun probe(f: File): Triple<Int, Int, Long> {
        val key = f.path + ":" + f.length()
        synchronized(probeCache) { probeCache[key]?.let { return it } }
        val r = MediaMetadataRetriever()
        val t = try {
            r.setDataSource(f.path)
            Triple(
                r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0,
                r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0,
                r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0L
            )
        } catch (_: Exception) { Triple(0, 0, 0L) } finally { try { r.release() } catch (_: Exception) {} }
        synchronized(probeCache) { probeCache[key] = t }
        return t
    }

    fun list(): List<StoredPhoto> {
        val metas = HashMap<String, JSONObject>()
        privDir.listFiles { f -> f.name.endsWith(".json") }.orEmpty().forEach { mf ->
            try {
                val m = JSONObject(mf.readText())
                if (m.has("file")) metas[m.getString("file")] = m
            } catch (_: Exception) {}
        }
        val files = publicDir.listFiles { f ->
            f.isFile && !f.name.startsWith(".") && f.extension.lowercase() in videoExt
        }.orEmpty()
        return files.map { f ->
            val m = metas[f.name]
            if (m != null && m.optInt("width") > 0) {
                val id = m.getString("id")
                StoredPhoto(
                    id, f, thumbFor(id),
                    m.optString("filename", f.name), m.optString("cameraMake"), m.optString("cameraModel"),
                    m.optInt("width"), m.optInt("height"),
                    m.optString("createTime").ifBlank { isoFormat.format(Date(f.lastModified())) },
                    m.optLong("durationMs")
                )
            } else {
                val id = m?.optString("id")?.takeIf { it.isNotBlank() } ?: ("local_" + f.name)
                val (w, h, d) = probe(f)
                StoredPhoto(
                    id, f, thumbFor(id), f.name, "", "", w, h,
                    m?.optString("createTime")?.takeIf { it.isNotBlank() } ?: isoFormat.format(Date(f.lastModified())), d
                )
            }
        }.sortedWith(compareBy<StoredPhoto>({ it.createTime }, { it.filename }))   // ältestes Video zuerst
    }

    fun delete(p: StoredPhoto): Boolean {
        val ok = !p.file.exists() || p.file.delete()
        p.thumb.delete(); metaFor(p.id).delete()
        if (ok) MediaScannerConnection.scanFile(ctx, arrayOf(p.file.absolutePath), null, null)
        return ok
    }

    /** Vorschaubild: ein Bild nach 1 Sekunde aus dem Video. */
    fun makeThumb(id: String, src: File) {
        val r = MediaMetadataRetriever()
        try {
            r.setDataSource(src.path)
            val bmp = r.getScaledFrameAtTime(1_000_000, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, 640, 640)
                ?: r.getFrameAtTime(0) ?: return
            thumbFor(id).outputStream().use { bmp.compress(Bitmap.CompressFormat.JPEG, 85, it) }
            bmp.recycle()
        } finally { try { r.release() } catch (_: Exception) {} }
    }

    /** In dieser App gibt es keine alten Daten zu verschieben. */
    fun migrateOld(): Int = 0
}
