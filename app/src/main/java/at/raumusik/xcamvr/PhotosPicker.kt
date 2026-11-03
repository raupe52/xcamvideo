package at.raumusik.xcamvr

import org.json.JSONObject
import java.net.URLEncoder

/** Google Photos Picker API (https://developers.google.com/photos/picker). */
class PhotosPicker(private val auth: GoogleAuth) {
    companion object { private const val BASE = "https://photospicker.googleapis.com/v1" }

    data class Session(val id: String, val pickerUri: String, val pollMs: Long, val itemsSet: Boolean)

    data class PickedItem(
        val id: String, val type: String, val baseUrl: String, val mimeType: String,
        val filename: String, val meta: JSONObject, val createTime: String
    )

    private fun parse(j: JSONObject): Session {
        val poll = j.optJSONObject("pollingConfig")?.optString("pollInterval", "5s") ?: "5s"
        val ms = ((poll.removeSuffix("s").toDoubleOrNull() ?: 5.0) * 1000).toLong().coerceIn(2000, 30000)
        return Session(j.getString("id"), j.optString("pickerUri"), ms, j.optBoolean("mediaItemsSet", false))
    }

    fun create(): Session = parse(Http.postJson("$BASE/sessions", JSONObject(), auth.accessToken()))

    fun get(id: String): Session = parse(Http.getJson("$BASE/sessions/$id", auth.accessToken()))

    fun delete(id: String) = try { Http.delete("$BASE/sessions/$id", auth.accessToken()) } catch (_: Exception) {}

    fun listItems(sessionId: String): List<PickedItem> {
        val out = mutableListOf<PickedItem>()
        var page: String? = null
        do {
            var url = "$BASE/mediaItems?sessionId=${URLEncoder.encode(sessionId, "UTF-8")}&pageSize=100"
            if (page != null) url += "&pageToken=" + URLEncoder.encode(page, "UTF-8")
            val j = Http.getJson(url, auth.accessToken())
            val arr = j.optJSONArray("mediaItems")
            if (arr != null) for (i in 0 until arr.length()) {
                val it = arr.getJSONObject(i)
                val mf = it.optJSONObject("mediaFile") ?: continue
                out += PickedItem(
                    id = it.getString("id"),
                    type = it.optString("type", "PHOTO"),
                    baseUrl = mf.optString("baseUrl"),
                    mimeType = mf.optString("mimeType"),
                    filename = mf.optString("filename"),
                    meta = mf.optJSONObject("mediaFileMetadata") ?: JSONObject(),
                    createTime = it.optString("createTime")
                )
            }
            page = j.optString("nextPageToken", "").ifBlank { null }
        } while (page != null)
        return out
    }

    /** Video laden ("=dv"), fortsetzbar, direkt in den öffentlichen Ordner. */
    fun download(item: PickedItem, store: PhotoStore, progress: (Long, Long) -> Unit) {
        val target = store.targetFor(item.id, item.filename)
        val part = store.partFor(target)
        Http.downloadResumable(item.baseUrl + "=dv", auth.accessToken(), part, target, progress)
        store.saveMeta(item.id, target, JSONObject()
            .put("filename", item.filename)
            .put("cameraMake", item.meta.optString("cameraMake"))
            .put("cameraModel", item.meta.optString("cameraModel"))
            .put("createTime", item.createTime))
        try { store.makeThumb(item.id, target) } catch (_: Exception) {}
    }
}
