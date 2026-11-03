package at.raumusik.xcamvr

import android.app.NativeActivity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.media.MediaMetadataRetriever
import android.media.MediaPlayer
import android.os.Bundle
import android.util.Log
import android.view.Surface
import java.io.File

/**
 * Immersiver Video-Player. Das Bild zeichnet die Quest selbst (OpenXR-Ebene mit Android-Surface,
 * siehe cpp/main.cpp); diese Klasse steuert den MediaPlayer und malt die Einblendungen.
 * Die Methoden werden per JNI aus dem nativen Thread aufgerufen.
 */
class VrActivity : NativeActivity() {

    companion object {
        const val EXTRA_PATHS = "paths"
        const val EXTRA_INDEX = "index"
        private const val TAG = "XcamVideo"
    }

    private var paths: List<String> = emptyList()
    private var startIndex = 0
    @Volatile private var jump = -1

    private val lock = Any()
    private var player: MediaPlayer? = null
    private var playingIndex = -1
    @Volatile private var prepared = false
    private var volume = 1f

    private val prefs by lazy { getSharedPreferences("viewer", MODE_PRIVATE) }
    private val photoPrefs by lazy { getSharedPreferences("photo_settings", MODE_PRIVATE) }
    private val positions by lazy { getSharedPreferences("positions", MODE_PRIVATE) }

    override fun onCreate(savedInstanceState: Bundle?) {
        paths = intent?.getStringArrayListExtra(EXTRA_PATHS) ?: emptyList()
        startIndex = intent?.getIntExtra(EXTRA_INDEX, 0) ?: 0
        volume = prefs.getFloat("volume", 1f)
        super.onCreate(savedInstanceState)
    }

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        val p = intent?.getStringArrayListExtra(EXTRA_PATHS) ?: return
        paths = p
        jump = intent.getIntExtra(EXTRA_INDEX, 0).coerceIn(0, maxOf(0, p.size - 1))
    }

    override fun onPause() {
        synchronized(lock) { player?.let { if (prepared && it.isPlaying) it.pause() }; savePosition() }
        super.onPause()
    }

    override fun onDestroy() {
        stopVideo()
        super.onDestroy()
    }

    // ---------------- JNI: Liste ----------------

    @Suppress("unused") fun getImageCount(): Int = paths.size
    @Suppress("unused") fun getStartIndex(): Int = startIndex.coerceIn(0, maxOf(0, paths.size - 1))
    @Suppress("unused") fun getImageName(index: Int): String = paths.getOrNull(index)?.let { File(it).name } ?: ""
    @Suppress("unused") fun consumeJump(): Int { val j = jump; jump = -1; return j }

    /** Breite und Höhe des Videos (für die Größe der OpenXR-Ebene). */
    @Suppress("unused")
    fun getVideoSize(index: Int): IntArray {
        val path = paths.getOrNull(index) ?: return intArrayOf(0, 0)
        val r = MediaMetadataRetriever()
        return try {
            r.setDataSource(path)
            var w = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
            var h = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
            val rot = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
            if (rot == 90 || rot == 270) { val t = w; w = h; h = t }
            intArrayOf(w, h)
        } catch (e: Exception) {
            Log.e(TAG, "Größe unbekannt: $path", e); intArrayOf(0, 0)
        } finally { try { r.release() } catch (_: Exception) {} }
    }

    // ---------------- JNI: Videobild als Textur ----------------

    private var surfaceTexture: android.graphics.SurfaceTexture? = null
    private var videoSurface: Surface? = null
    @Volatile private var frameAvailable = false

    /** Erzeugt (im GL-Thread) eine SurfaceTexture auf der externen Textur [tex]. */
    @Suppress("unused")
    fun createVideoSurface(tex: Int, w: Int, h: Int): Surface? {
        releaseVideoSurface()
        return try {
            val st = android.graphics.SurfaceTexture(tex)
            st.setDefaultBufferSize(w, h)
            st.setOnFrameAvailableListener { frameAvailable = true }
            surfaceTexture = st
            Surface(st).also { videoSurface = it }
        } catch (e: Exception) { Log.e(TAG, "SurfaceTexture", e); null }
    }

    /** Neues Bild in die Textur holen (im GL-Thread). Liefert die Transformationsmatrix in [m]. */
    @Suppress("unused")
    fun updateVideoTexture(m: FloatArray): Boolean {
        if (!frameAvailable) return false
        frameAvailable = false
        val st = surfaceTexture ?: return false
        return try { st.updateTexImage(); st.getTransformMatrix(m); true } catch (e: Exception) { false }
    }

    @Suppress("unused")
    fun releaseVideoSurface() {
        try { videoSurface?.release() } catch (_: Exception) {}
        try { surfaceTexture?.release() } catch (_: Exception) {}
        videoSurface = null; surfaceTexture = null; frameAvailable = false
    }

    // ---------------- JNI: Wiedergabe ----------------

    /** Startet das Video in die von OpenXR gelieferte Surface (setzt an der letzten Stelle fort). */
    @Suppress("unused")
    fun startVideo(index: Int, surface: Surface) {
        stopVideo()
        val path = paths.getOrNull(index) ?: return
        synchronized(lock) {
            try {
                val mp = MediaPlayer()
                player = mp; playingIndex = index; prepared = false
                mp.setSurface(surface)
                mp.setDataSource(path)
                mp.isLooping = true
                mp.setVolume(volume, volume)
                mp.setOnPreparedListener {
                    synchronized(lock) {
                        if (player !== it) return@synchronized
                        prepared = true
                        val pos = positions.getInt(File(path).name, 0)
                        if (pos in 1000 until (it.duration - 3000)) it.seekTo(pos)
                        it.start()
                    }
                }
                mp.setOnErrorListener { _, what, extra ->
                    Log.e(TAG, "Wiedergabefehler $what/$extra: $path"); true
                }
                mp.prepareAsync()
            } catch (e: Exception) {
                Log.e(TAG, "Kann Video nicht starten: $path", e)
            }
        }
    }

    @Suppress("unused")
    fun stopVideo() {
        synchronized(lock) {
            savePosition()
            player?.let { try { it.stop() } catch (_: Exception) {}; try { it.release() } catch (_: Exception) {} }
            player = null; prepared = false; playingIndex = -1
        }
    }

    private fun savePosition() {
        val mp = player ?: return
        val path = paths.getOrNull(playingIndex) ?: return
        if (!prepared) return
        try {
            val pos = mp.currentPosition
            // Am Ende angekommen → nächstes Mal von vorne
            val v = if (pos > mp.duration - 3000) 0 else pos
            positions.edit().putInt(File(path).name, v).apply()
        } catch (_: Exception) {}
    }

    /** Play/Pause. Gibt true zurück, wenn jetzt abgespielt wird. */
    @Suppress("unused")
    fun togglePause(): Boolean = synchronized(lock) {
        val mp = player ?: return false
        if (!prepared) return false
        if (mp.isPlaying) { mp.pause(); savePosition(); false } else { mp.start(); true }
    }

    @Suppress("unused")
    fun seekBy(ms: Int) {
        synchronized(lock) {
            val mp = player ?: return
            if (!prepared) return
            val target = (mp.currentPosition + ms).coerceIn(0, maxOf(0, mp.duration - 500))
            mp.seekTo(target)
        }
    }

    @Suppress("unused") fun positionMs(): Int = synchronized(lock) { if (prepared) player?.currentPosition ?: 0 else 0 }
    @Suppress("unused") fun durationMs(): Int = synchronized(lock) { if (prepared) player?.duration ?: 0 else 0 }
    @Suppress("unused") fun isPlaying(): Boolean = synchronized(lock) { prepared && player?.isPlaying == true }
    @Suppress("unused") fun isPrepared(): Boolean = prepared

    @Suppress("unused") fun getVolume(): Float = volume
    @Suppress("unused")
    fun setVolume(v: Float) {
        volume = v.coerceIn(0f, 1f)
        prefs.edit().putFloat("volume", volume).apply()
        synchronized(lock) { player?.setVolume(volume, volume) }
    }

    // ---------------- JNI: Einstellungen ----------------

    @Suppress("unused") fun getBrightness(): Float = 1f
    @Suppress("unused") fun setBrightness(v: Float) {}
    @Suppress("unused") fun getSetting(key: String, def: Int): Int = prefs.getInt(key, def)
    @Suppress("unused") fun putSetting(key: String, v: Int) { prefs.edit().putInt(key, v).apply() }

    @Suppress("unused") fun getPhotoSettings(index: Int): String =
        paths.getOrNull(index)?.let { photoPrefs.getString(File(it).name, "") } ?: ""
    @Suppress("unused") fun savePhotoSettings(index: Int, v: String) {
        val p = paths.getOrNull(index) ?: return
        photoPrefs.edit().putString(File(p).name, v).apply()
    }

    /** Zurück zur Übersicht (2D-Fenster) statt die App zu beenden. */
    @Suppress("unused") fun openGallery() {
        runOnUiThread {
            stopVideo()
            try {
                startActivity(android.content.Intent(this, MainActivity::class.java)
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK or android.content.Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
            } catch (e: Exception) { Log.e(TAG, "Übersicht", e) }
            finish()
        }
    }

    // Aus der Foto-App übernommen, hier nicht benutzt
    @Suppress("unused") fun requestImage(index: Int, maxSize: Int) {}
    @Suppress("unused") fun pollImage(): Bitmap? = null

    // ---------------- Einblendungen ----------------

    @Suppress("unused")
    fun makeLabel(line1: String, line2: String): Bitmap {
        val w = 1024; val h = 160
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(200, 20, 28, 38) }
        c.drawRoundRect(RectF(0f, 0f, w.toFloat(), h.toFloat()), 36f, 36f, bg)
        val p1 = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textSize = 54f; textAlign = Paint.Align.CENTER }
        val p2 = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(242, 179, 61); textSize = 38f; textAlign = Paint.Align.CENTER }
        fit(line1, p1, w - 60f, 30f); fit(line2, p2, w - 60f, 24f)
        c.drawText(ellipsize(line1, p1, w - 60f), w / 2f, 72f, p1)
        c.drawText(ellipsize(line2, p2, w - 60f), w / 2f, 130f, p2)
        return bmp
    }

    private fun fit(s: String, p: Paint, max: Float, minSize: Float) {
        while (p.textSize > minSize && p.measureText(s) > max) p.textSize -= 2f
    }

    private fun ellipsize(s: String, p: Paint, max: Float): String {
        if (p.measureText(s) <= max) return s
        var t = s
        while (t.isNotEmpty() && p.measureText("$t…") > max) t = t.dropLast(1)
        return "$t…"
    }
}
