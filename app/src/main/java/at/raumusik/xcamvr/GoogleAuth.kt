package at.raumusik.xcamvr

import android.content.Context
import android.net.Uri
import android.util.Base64
import java.net.InetAddress
import java.net.ServerSocket
import java.net.URLDecoder
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Google-Anmeldung für "Desktop-App"-OAuth-Clients mit Loopback-Redirect (http://127.0.0.1:PORT)
 * und PKCE. Funktioniert auf der Quest ohne Google-Play-Dienste: Der Quest-Browser zeigt die
 * Google-Anmeldung, leitet danach auf 127.0.0.1 um, und diese App nimmt den Code entgegen.
 */
class GoogleAuth(private val ctx: Context) {

    companion object {
        const val SCOPE = "https://www.googleapis.com/auth/photospicker.mediaitems.readonly"
        private const val AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
        private const val TOKEN_URL = "https://oauth2.googleapis.com/token"
        private const val PREFS = "auth"
    }

    private val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    var clientId: String
        get() = prefs.getString("client_id", null)?.takeIf { it.isNotBlank() } ?: BuildConfig.GOOGLE_CLIENT_ID
        set(v) = prefs.edit().putString("client_id", v.trim()).apply()

    var clientSecret: String
        get() = prefs.getString("client_secret", null)?.takeIf { it.isNotBlank() } ?: BuildConfig.GOOGLE_CLIENT_SECRET
        set(v) = prefs.edit().putString("client_secret", v.trim()).apply()

    val isConfigured: Boolean get() = clientId.isNotBlank()
    val isSignedIn: Boolean get() = prefs.getString("refresh_token", null) != null

    @Volatile private var server: ServerSocket? = null

    private fun randomString(bytes: Int): String {
        val b = ByteArray(bytes); SecureRandom().nextBytes(b)
        return Base64.encodeToString(b, Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP)
    }

    /**
     * Startet einen lokalen Empfänger und liefert die Anmelde-URL. [onDone] wird (in einem
     * Hintergrund-Thread) mit null bei Erfolg oder mit einer Fehlermeldung aufgerufen.
     */
    fun beginSignIn(onDone: (String?) -> Unit): String {
        cancelSignIn()
        val socket = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))
        server = socket
        val redirect = "http://127.0.0.1:${socket.localPort}"
        val verifier = randomString(48)
        val challenge = Base64.encodeToString(
            MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray(Charsets.US_ASCII)),
            Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP
        )
        val state = randomString(16)

        Thread {
            var result: String? = "Anmeldung abgebrochen"
            try {
                while (true) {
                    val client = socket.accept()
                    client.soTimeout = 15_000
                    val reader = client.getInputStream().bufferedReader()
                    val requestLine = reader.readLine() ?: ""
                    // Restliche Header lesen
                    while (true) { val l = reader.readLine() ?: break; if (l.isEmpty()) break }
                    val path = requestLine.split(" ").getOrNull(1) ?: "/"
                    if (path.startsWith("/favicon")) {
                        client.getOutputStream().write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".toByteArray())
                        client.close(); continue
                    }
                    val params = parseQuery(path.substringAfter('?', ""))
                    var ok = false
                    result = when {
                        params["error"] != null -> "Google meldet: ${params["error"]}"
                        params["state"] != state -> "Ungültige Antwort (state)"
                        params["code"] == null -> "Kein Code erhalten"
                        else -> try {
                            exchangeCode(params["code"]!!, verifier, redirect); ok = true; null
                        } catch (e: Exception) { "Token-Fehler: ${e.message}" }
                    }
                    val html = if (ok)
                        "<h2>Angemeldet ✔</h2><p>Du kannst dieses Browserfenster schließen und zur App <b>XCAM VR Videos</b> zurückkehren.</p>"
                    else "<h2>Anmeldung fehlgeschlagen</h2><p>${result ?: ""}</p>"
                    val page = "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'></head>" +
                        "<body style='font-family:sans-serif;background:#1e2a38;color:#fff;padding:40px;font-size:22px'>$html</body></html>"
                    val bytes = page.toByteArray()
                    client.getOutputStream().apply {
                        write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ${bytes.size}\r\nConnection: close\r\n\r\n".toByteArray())
                        write(bytes); flush()
                    }
                    client.close()
                    break
                }
            } catch (e: Exception) {
                if (server === socket) result = "Anmeldung unterbrochen: ${e.message}"
            } finally {
                try { socket.close() } catch (_: Exception) {}
                if (server === socket) { server = null; onDone(result) }
            }
        }.start()

        return Uri.parse(AUTH_URL).buildUpon()
            .appendQueryParameter("client_id", clientId)
            .appendQueryParameter("redirect_uri", redirect)
            .appendQueryParameter("response_type", "code")
            .appendQueryParameter("scope", SCOPE)
            .appendQueryParameter("code_challenge", challenge)
            .appendQueryParameter("code_challenge_method", "S256")
            .appendQueryParameter("access_type", "offline")
            .appendQueryParameter("prompt", "consent")
            .appendQueryParameter("state", state)
            .build().toString()
    }

    fun cancelSignIn() {
        val s = server; server = null
        try { s?.close() } catch (_: Exception) {}
    }

    private fun parseQuery(q: String): Map<String, String> =
        q.split("&").filter { it.contains("=") }.associate {
            URLDecoder.decode(it.substringBefore("="), "UTF-8") to URLDecoder.decode(it.substringAfter("="), "UTF-8")
        }

    private fun exchangeCode(code: String, verifier: String, redirect: String) {
        val form = mutableMapOf(
            "code" to code,
            "client_id" to clientId,
            "redirect_uri" to redirect,
            "grant_type" to "authorization_code",
            "code_verifier" to verifier
        )
        if (clientSecret.isNotBlank()) form["client_secret"] = clientSecret
        val json = Http.postForm(TOKEN_URL, form)
        val refresh = json.optString("refresh_token", "")
        val edit = prefs.edit()
        if (refresh.isNotBlank()) edit.putString("refresh_token", refresh)
        edit.putString("access_token", json.getString("access_token"))
            .putLong("expires_at", System.currentTimeMillis() + json.optLong("expires_in", 3600) * 1000)
            .apply()
        if (refresh.isBlank() && prefs.getString("refresh_token", null) == null)
            throw IllegalStateException("Google hat kein Refresh-Token geliefert")
    }

    /** Gültiges Access-Token holen (blockierend, nicht im UI-Thread aufrufen). */
    @Synchronized
    fun accessToken(): String {
        val token = prefs.getString("access_token", null)
        val exp = prefs.getLong("expires_at", 0)
        if (token != null && System.currentTimeMillis() < exp - 60_000) return token
        val refresh = prefs.getString("refresh_token", null)
            ?: throw IllegalStateException("Nicht angemeldet")
        val form = mutableMapOf(
            "client_id" to clientId,
            "refresh_token" to refresh,
            "grant_type" to "refresh_token"
        )
        if (clientSecret.isNotBlank()) form["client_secret"] = clientSecret
        try {
            val json = Http.postForm(TOKEN_URL, form)
            val newToken = json.getString("access_token")
            prefs.edit().putString("access_token", newToken)
                .putLong("expires_at", System.currentTimeMillis() + json.optLong("expires_in", 3600) * 1000)
                .apply()
            return newToken
        } catch (e: HttpException) {
            if (e.code == 400 || e.code == 401) {
                signOut()
                throw IllegalStateException("Anmeldung abgelaufen – bitte neu anmelden")
            }
            throw e
        }
    }

    fun signOut() {
        prefs.edit().remove("refresh_token").remove("access_token").remove("expires_at").apply()
    }
}
