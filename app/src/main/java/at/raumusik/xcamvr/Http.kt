package at.raumusik.xcamvr

import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

class HttpException(val code: Int, message: String) : IOException(message)

object Http {
    private fun open(url: String, method: String, token: String?): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = 20_000
        c.readTimeout = 60_000
        if (token != null) c.setRequestProperty("Authorization", "Bearer $token")
        return c
    }

    private fun readBody(c: HttpURLConnection): String {
        val code = c.responseCode
        val stream = if (code in 200..299) c.inputStream else c.errorStream
        val text = stream?.bufferedReader()?.use { it.readText() } ?: ""
        if (code !in 200..299) throw HttpException(code, "HTTP $code: ${text.take(400)}")
        return text
    }

    fun getJson(url: String, token: String? = null): JSONObject {
        val c = open(url, "GET", token)
        try { return JSONObject(readBody(c).ifBlank { "{}" }) } finally { c.disconnect() }
    }

    fun postJson(url: String, body: JSONObject, token: String? = null): JSONObject {
        val c = open(url, "POST", token)
        try {
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/json")
            c.outputStream.use { it.write(body.toString().toByteArray()) }
            return JSONObject(readBody(c).ifBlank { "{}" })
        } finally { c.disconnect() }
    }

    fun postForm(url: String, form: Map<String, String>): JSONObject {
        val c = open(url, "POST", null)
        try {
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            val body = form.entries.joinToString("&") {
                URLEncoder.encode(it.key, "UTF-8") + "=" + URLEncoder.encode(it.value, "UTF-8")
            }
            c.outputStream.use { it.write(body.toByteArray()) }
            return JSONObject(readBody(c).ifBlank { "{}" })
        } finally { c.disconnect() }
    }

    fun delete(url: String, token: String?) {
        val c = open(url, "DELETE", token)
        try { readBody(c) } finally { c.disconnect() }
    }

    /**
     * Großen Download fortsetzbar laden: schreibt in [part] (setzt dort fort, falls schon
     * teilweise vorhanden) und benennt am Ende in [target] um. [progress] bekommt (geladen, gesamt).
     */
    fun downloadResumable(url: String, token: String?, part: File, target: File, progress: (Long, Long) -> Unit) {
        val have = if (part.exists()) part.length() else 0L
        val c = open(url, "GET", token)
        c.readTimeout = 120_000
        if (have > 0) c.setRequestProperty("Range", "bytes=$have-")
        try {
            val code = c.responseCode
            if (code !in 200..299) {
                val err = c.errorStream?.bufferedReader()?.use { it.readText() } ?: ""
                throw HttpException(code, "Download HTTP $code: ${err.take(300)}")
            }
            val append = code == 206 && have > 0
            val start = if (append) have else 0L
            val len = c.getHeaderField("Content-Length")?.toLongOrNull() ?: -1L
            val total = if (len >= 0) start + len else -1L
            var done = start
            var lastReport = 0L
            java.io.FileOutputStream(part, append).use { out ->
                c.inputStream.use { input ->
                    val buf = ByteArray(1024 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        val now = System.currentTimeMillis()
                        if (now - lastReport > 1000) { progress(done, total); lastReport = now }
                    }
                }
            }
            if (total >= 0 && done < total) throw IOException("Download unvollständig ($done von $total Bytes)")
            target.delete()
            if (!part.renameTo(target)) {
                part.inputStream().use { i -> target.outputStream().use { o -> i.copyTo(o, 1024 * 1024) } }
                part.delete()
            }
        } finally { c.disconnect() }
    }

    /** Lädt eine Datei herunter (zuerst in .part, dann umbenennen). */
    fun download(url: String, token: String?, target: File) {
        val c = open(url, "GET", token)
        try {
            val code = c.responseCode
            if (code !in 200..299) {
                val err = c.errorStream?.bufferedReader()?.use { it.readText() } ?: ""
                throw HttpException(code, "Download HTTP $code: ${err.take(300)}")
            }
            val part = File(target.path + ".part")
            c.inputStream.use { input -> part.outputStream().use { input.copyTo(it, 256 * 1024) } }
            if (!part.renameTo(target)) throw IOException("Konnte ${target.name} nicht speichern")
        } finally { c.disconnect() }
    }
}
