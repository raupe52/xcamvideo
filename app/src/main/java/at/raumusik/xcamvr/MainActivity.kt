package at.raumusik.xcamvr

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.util.LruCache
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.*
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import kotlin.concurrent.thread

class MainActivity : Activity() {

    private lateinit var auth: GoogleAuth
    private lateinit var picker: PhotosPicker
    private lateinit var store: PhotoStore

    private lateinit var status: TextView
    private lateinit var signInBtn: Button
    private lateinit var pickBtn: Button
    private lateinit var pickerPanel: LinearLayout
    private lateinit var qrView: ImageView
    private lateinit var xcamOnly: CheckBox
    private lateinit var countText: TextView
    private lateinit var grid: GridView

    private var photos: List<StoredPhoto> = emptyList()
    @Volatile private var pickerCancelled = false
    @Volatile private var busy = false
    private var currentPickerUri: String? = null

    private val thumbExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()
    private val thumbJobs = java.util.Collections.synchronizedSet(HashSet<String>())

    private val thumbCache = object : LruCache<String, Bitmap>(48 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }

    private fun dp(v: Int) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        auth = GoogleAuth(this)
        picker = PhotosPicker(auth)
        store = PhotoStore(this)
        buildUi()
        refresh()
        val needed = mutableListOf<String>()
        if (checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED)
            needed += android.Manifest.permission.READ_EXTERNAL_STORAGE
        if (android.os.Build.VERSION.SDK_INT <= 29 &&
            checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED)
            needed += android.Manifest.permission.WRITE_EXTERNAL_STORAGE
        if (needed.isNotEmpty()) requestPermissions(needed.toTypedArray(), 1) else migrate()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        migrate()
    }

    /** Fotos der ersten Version einmalig in den Ordner Movies/XCAM verschieben. */
    private fun migrate() {
        thread {
            val n = try { store.migrateOld() } catch (_: Exception) { 0 }
            runOnUiThread {
                refresh()
                if (n > 0) toast("$n Videos verschoben")
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (!busy) refresh() else updateButtons()
    }

    // ---------------------------------------------------------------- UI

    private fun button(text: String, onClick: () -> Unit) = Button(this).apply {
        this.text = text; isAllCaps = false; textSize = 16f
        setOnClickListener { onClick() }
    }

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(16), dp(20), dp(8))
            setBackgroundColor(Color.parseColor("#141C26"))
        }

        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        header.addView(TextView(this).apply {
            text = "XCAM VR Videos"; textSize = 26f; setTextColor(Color.parseColor("#F2B33D"))
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        pickBtn = button("Aus Google Fotos wählen") { startPicking() }
        signInBtn = button("Anmelden") { if (auth.isSignedIn) confirmSignOut() else signIn() }
        header.addView(pickBtn)
        header.addView(signInBtn)
        header.addView(button("⚙") { showSettings() })
        root.addView(header)

        status = TextView(this).apply { textSize = 15f; setTextColor(Color.LTGRAY); setPadding(0, dp(8), 0, dp(8)) }
        root.addView(status)

        // Picker-Bereich mit QR-Code
        pickerPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; visibility = View.GONE
            setPadding(dp(12), dp(12), dp(12), dp(12)); setBackgroundColor(Color.parseColor("#1E2A38"))
        }
        qrView = ImageView(this)
        pickerPanel.addView(qrView, LinearLayout.LayoutParams(dp(220), dp(220)))
        val pickCol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), 0, 0, 0) }
        pickCol.addView(TextView(this).apply {
            textSize = 16f; setTextColor(Color.WHITE)
            text = "Wähle jetzt die XCAM-Videos in Google Fotos aus:\n\n" +
                "• Am Handy: QR-Code scannen, Videos (oder ein Album) auswählen, „Fertig“ tippen.\n" +
                "• Oder hier in der Quest: im Browser öffnen.\n\n" +
                "Die App wartet und lädt die Auswahl danach automatisch herunter."
        })
        val pickBtns = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, dp(12), 0, 0) }
        pickBtns.addView(button("Im Quest-Browser öffnen") { currentPickerUri?.let { openUrl(it) } })
        pickBtns.addView(button("Abbrechen") { pickerCancelled = true })
        pickCol.addView(pickBtns)
        pickerPanel.addView(pickCol, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(pickerPanel)

        val bar = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(0, dp(8), 0, dp(8)) }
        xcamOnly = CheckBox(this).apply {
            text = "Nur XCAM-Videos"; setTextColor(Color.WHITE)
            isChecked = getPreferences(MODE_PRIVATE).getBoolean("xcamOnly", false)
            setOnCheckedChangeListener { _, c -> getPreferences(MODE_PRIVATE).edit().putBoolean("xcamOnly", c).apply(); refresh() }
        }
        bar.addView(xcamOnly)
        countText = TextView(this).apply { setTextColor(Color.LTGRAY); setPadding(dp(16), 0, 0, 0) }
        bar.addView(countText, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        bar.addView(button("▶ Alle in VR ansehen") { if (photos.isNotEmpty()) startVr(0) })
        root.addView(bar)

        grid = GridView(this).apply {
            numColumns = GridView.AUTO_FIT
            columnWidth = dp(280)
            stretchMode = GridView.STRETCH_COLUMN_WIDTH
            horizontalSpacing = dp(10); verticalSpacing = dp(10)
            adapter = PhotoAdapter()
            setOnItemClickListener { _, _, pos, _ -> startVr(pos) }
            setOnItemLongClickListener { _, _, pos, _ -> confirmDelete(photos[pos]); true }
        }
        root.addView(grid, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        setContentView(root)
    }

    private fun updateButtons() {
        signInBtn.text = if (auth.isSignedIn) "Abmelden" else "Anmelden"
        pickBtn.isEnabled = auth.isSignedIn && !busy
        if (!busy) status.text = lastMessage ?: when {
            !auth.isConfigured -> "Zuerst unter ⚙ die Google-Client-ID eintragen (siehe Anleitung)."
            !auth.isSignedIn -> "Bitte mit deinem Google-Konto anmelden."
            photos.isEmpty() -> "Noch keine Videos – „Aus Google Fotos wählen“ oder per USB in den Ordner Movies/XCAM kopieren."
            else -> "Video antippen = in VR abspielen · lange drücken = löschen"
        }
    }

    private fun refresh() {
        val all = store.list()
        photos = if (xcamOnly.isChecked) all.filter { it.isXcam } else all
        countText.text = if (photos.size == all.size) "${all.size} Videos" else "${photos.size} von ${all.size} Videos"
        (grid.adapter as BaseAdapter).notifyDataSetChanged()
        updateButtons()
    }

    private fun mb(b: Long) = if (b >= 1_000_000_000L) String.format(java.util.Locale.GERMAN, "%.2f GB", b / 1e9)
                              else String.format(java.util.Locale.GERMAN, "%.0f MB", b / 1e6)

    private fun dur(ms: Long): String {
        val s = ms / 1000
        return if (s >= 3600) String.format("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60) else String.format("%d:%02d", s / 60, s % 60)
    }

    private fun setStatus(s: String) = runOnUiThread { status.text = s }

    /** Letzte Meldung bleibt in der Statuszeile stehen, bis die nächste Aktion beginnt. */
    @Volatile private var lastMessage: String? = null

    private fun toast(s: String) {
        lastMessage = s
        runOnUiThread {
            status.text = s
            Toast.makeText(this, s, Toast.LENGTH_LONG).show()
            if (s.startsWith("Fehler")) {
                AlertDialog.Builder(this).setTitle("Es ist ein Fehler aufgetreten")
                    .setMessage(s).setPositiveButton("OK", null).show()
            }
        }
    }

    private fun openUrl(url: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        } catch (e: Exception) { toast("Kein Browser gefunden: ${e.message}") }
    }

    // ---------------------------------------------------------------- Anmeldung

    private fun signIn() {
        if (!auth.isConfigured) { showSettings(); return }
        val url = auth.beginSignIn { err ->
            runOnUiThread {
                if (err == null) {
                    toast("Angemeldet ✔")
                    // App wieder nach vorne holen
                    startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
                } else toast(err)
                updateButtons()
            }
        }
        status.text = "Anmeldung läuft im Browser …"
        openUrl(url)
    }

    private fun confirmSignOut() {
        AlertDialog.Builder(this).setTitle("Abmelden?")
            .setMessage("Die bereits geladenen Videos bleiben erhalten.")
            .setPositiveButton("Abmelden") { _, _ -> auth.signOut(); updateButtons() }
            .setNegativeButton("Abbrechen", null).show()
    }

    private fun showSettings() {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), 0) }
        val idField = EditText(this).apply { hint = "Client-ID (…apps.googleusercontent.com)"; setText(auth.clientId); setSingleLine() }
        val secretField = EditText(this).apply {
            hint = "Clientschlüssel (Client secret)"; setText(auth.clientSecret); setSingleLine()
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        }
        col.addView(TextView(this).apply { text = "OAuth-Client vom Typ „Desktop-App“ aus der Google Cloud Console:" })
        col.addView(idField); col.addView(secretField)
        AlertDialog.Builder(this).setTitle("Google-Zugang").setView(col)
            .setPositiveButton("Speichern") { _, _ ->
                auth.clientId = idField.text.toString(); auth.clientSecret = secretField.text.toString(); updateButtons()
            }
            .setNegativeButton("Abbrechen", null).show()
    }

    // ---------------------------------------------------------------- Auswahl & Download

    private fun qrBitmap(text: String): Bitmap {
        val size = 600
        val m = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, size, size, mapOf(EncodeHintType.MARGIN to 2))
        val px = IntArray(size * size) { i -> if (m[i % size, i / size]) Color.BLACK else Color.WHITE }
        return Bitmap.createBitmap(px, size, size, Bitmap.Config.ARGB_8888)
    }

    private fun startPicking() {
        if (busy) return
        busy = true; pickerCancelled = false; lastMessage = null; updateButtons()
        setStatus("Verbinde mit Google Fotos …")
        // Auch bei abgesetzter Brille / Ruhemodus weiterladen
        val wake = (getSystemService(POWER_SERVICE) as android.os.PowerManager)
            .newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "XcamVideo:download")
        val wifi = (applicationContext.getSystemService(WIFI_SERVICE) as android.net.wifi.WifiManager)
            .createWifiLock(android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF, "XcamVideo:download")
        thread {
            var sessionId: String? = null
            try {
                wake.acquire(4 * 60 * 60 * 1000L)
                wifi.acquire()
                KeepAliveService.start(this, "Google Fotos: Videos werden geladen")
                var s = picker.create()
                sessionId = s.id
                val uri = s.pickerUri.trimEnd('/') + "/autoclose"
                currentPickerUri = uri
                val qr = qrBitmap(s.pickerUri)
                runOnUiThread { qrView.setImageBitmap(qr); pickerPanel.visibility = View.VISIBLE }
                setStatus("Warte auf deine Auswahl in Google Fotos …")
                val deadline = System.currentTimeMillis() + 30 * 60_000
                while (!s.itemsSet) {
                    if (pickerCancelled) throw InterruptedException("abgebrochen")
                    if (System.currentTimeMillis() > deadline) throw IllegalStateException("Zeit für die Auswahl abgelaufen")
                    Thread.sleep(s.pollMs)
                    s = picker.get(s.id)
                }
                runOnUiThread { pickerPanel.visibility = View.GONE }
                setStatus("Auswahl erhalten, lade Liste …")
                val items = picker.listItems(s.id)
                var images = items.filter { it.type == "VIDEO" || it.mimeType.startsWith("video/") }
                val skipped = items.size - images.size
                var listedAt = System.currentTimeMillis()
                // Google-Links gelten nur 60 Minuten → bei Bedarf frisch holen
                fun relist() {
                    val fresh = picker.listItems(s.id).associateBy { it.id }
                    images = images.map { fresh[it.id] ?: it }
                    listedAt = System.currentTimeMillis()
                }
                var done = 0; var failed = 0
                for (idx in images.indices) {
                    if (pickerCancelled) break
                    done++
                    if (store.exists(images[idx].id)) continue
                    if (System.currentTimeMillis() - listedAt > 45 * 60_000L) {
                        try { relist() } catch (e: Exception) { android.util.Log.w("XcamVideo", "Neu auflisten", e) }
                    }
                    val label = "Lade Video $done von ${images.size}: ${images[idx].filename}"
                    setStatus(label)
                    var ok = false
                    for (attempt in 1..3) {
                        try {
                            picker.download(images[idx], store) { got, total ->
                                setStatus(label + " – " + mb(got) + (if (total > 0) " von " + mb(total) else ""))
                            }
                            ok = true; break
                        }
                        catch (e: HttpException) {
                            android.util.Log.w("XcamVideo", "Download HTTP ${e.code}", e)
                            if (e.code == 401 || e.code == 403 || e.code == 404) { try { relist() } catch (_: Exception) {} }
                            else Thread.sleep(3000)
                        } catch (e: Exception) {
                            android.util.Log.w("XcamVideo", "Download", e)
                            Thread.sleep(3000)
                        }
                    }
                    if (!ok) failed++
                    runOnUiThread { refresh() }
                }
                val msg = buildString {
                    append("${images.size - failed} Videos geladen.")
                    if (failed > 0) append(" $failed fehlgeschlagen.")
                    if (skipped > 0) append(" $skipped Fotos übersprungen (nur Videos).")
                }
                toast(msg)
            } catch (e: InterruptedException) {
                toast("Auswahl abgebrochen")
            } catch (e: Exception) {
                android.util.Log.e("XcamVideo", "Picker", e)
                toast("Fehler: ${e.javaClass.simpleName}: ${e.message}")
            } finally {
                sessionId?.let { picker.delete(it) }
                KeepAliveService.stop(this)
                try { if (wifi.isHeld) wifi.release() } catch (_: Exception) {}
                try { if (wake.isHeld) wake.release() } catch (_: Exception) {}
                currentPickerUri = null
                busy = false
                runOnUiThread { pickerPanel.visibility = View.GONE; refresh() }
            }
        }
    }

    private fun confirmDelete(p: StoredPhoto) {
        AlertDialog.Builder(this).setTitle("Video löschen?")
            .setMessage("${p.filename}\n\nWird aus dem Ordner Movies/XCAM auf der Quest gelöscht. In Google Fotos bleibt es erhalten.")
            .setPositiveButton("Löschen") { _, _ ->
                if (!store.delete(p)) toast("Konnte die Datei nicht löschen – bitte über die Dateien-App löschen.")
                thumbCache.remove(p.id); refresh()
            }
            .setNegativeButton("Abbrechen", null).show()
    }

    // ---------------------------------------------------------------- VR

    private fun startVr(index: Int) {
        val paths = ArrayList(photos.map { it.file.absolutePath })
        startActivity(Intent(this, VrActivity::class.java)
            .putStringArrayListExtra(VrActivity.EXTRA_PATHS, paths)
            .putExtra(VrActivity.EXTRA_INDEX, index)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    // ---------------------------------------------------------------- Galerie

    private inner class PhotoAdapter : BaseAdapter() {
        override fun getCount() = photos.size
        override fun getItem(position: Int) = photos[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
            val cell = (convertView as? LinearLayout) ?: LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                setBackgroundColor(Color.parseColor("#1E2A38"))
                setPadding(dp(4), dp(4), dp(4), dp(6))
                addView(ImageView(context).apply { scaleType = ImageView.ScaleType.CENTER_CROP },
                    LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(140)))
                addView(TextView(context).apply { setTextColor(Color.WHITE); textSize = 13f; maxLines = 2 })
            }
            val p = photos[position]
            val img = cell.getChildAt(0) as ImageView
            val txt = cell.getChildAt(1) as TextView
            var bmp = thumbCache.get(p.id)
            if (bmp == null && p.thumb.exists()) {
                bmp = BitmapFactory.decodeFile(p.thumb.path)
                if (bmp != null) thumbCache.put(p.id, bmp)
            } else if (bmp == null && thumbJobs.add(p.id)) {
                // Vorschaubild fehlt (z. B. selbst hineinkopiertes Foto) → im Hintergrund erzeugen
                thumbExecutor.execute {
                    try { store.makeThumb(p.id, p.file) } catch (_: Throwable) {}
                    runOnUiThread { notifyDataSetChanged() }
                }
            }
            img.setImageBitmap(bmp)
            val res = listOf(if (p.width > 0) "${p.width}×${p.height}" else "", if (p.durationMs > 0) dur(p.durationMs) else "")
                .filter { it.isNotBlank() }.joinToString(" · ")
            txt.text = listOf(p.filename, listOf(p.cameraLabel, res).filter { it.isNotBlank() }.joinToString(" · "))
                .filter { it.isNotBlank() }.joinToString("\n")
            return cell
        }
    }
}
