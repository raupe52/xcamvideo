package at.raumusik.xcamvr

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder

/**
 * Vordergrund-Dienst, solange Fotos heruntergeladen werden. Er sorgt dafür, dass die Quest
 * den Download nicht anhält, wenn die Brille abgesetzt wird oder in den Ruhemodus geht.
 */
class KeepAliveService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("download", "Downloads", NotificationManager.IMPORTANCE_LOW))
        val n = Notification.Builder(this, "download")
            .setContentTitle(getString(R.string.app_name))
            .setContentText(intent?.getStringExtra("text") ?: "Lade Videos …")
            .setSmallIcon(R.drawable.ic_launcher)
            .setOngoing(true)
            .build()
        startForeground(1, n)
        return START_NOT_STICKY
    }

    companion object {
        fun start(ctx: Context, text: String) {
            try { ctx.startForegroundService(Intent(ctx, KeepAliveService::class.java).putExtra("text", text)) }
            catch (e: Exception) { android.util.Log.w("KeepAlive", "start", e) }
        }
        fun stop(ctx: Context) {
            try { ctx.stopService(Intent(ctx, KeepAliveService::class.java)) } catch (_: Exception) {}
        }
    }
}
