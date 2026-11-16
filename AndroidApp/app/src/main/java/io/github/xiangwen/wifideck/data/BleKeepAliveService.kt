package io.github.xiangwen.wifideck.data

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import io.github.xiangwen.wifideck.MainActivity
import io.github.xiangwen.wifideck.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * 3C 前台保活服务 (`connectedDevice`)：
 * 在设备连接/重连期间保持常驻低优先级通知，防止熄屏或切后台后系统回收 BLE 链路；
 * 用户点击通知「断开」动作或连接彻底进入空闲 DISCONNECTED 时自动停止。
 */
class BleKeepAliveService : Service() {

    companion object {
        private const val TAG = "BleKeepAliveService"
        private const val CHANNEL_ID = "wifideck_ble_keepalive"
        private const val NOTIFICATION_ID = 1001

        const val ACTION_START = "io.github.xiangwen.wifideck.action.START_KEEP_ALIVE"
        const val ACTION_STOP = "io.github.xiangwen.wifideck.action.STOP_KEEP_ALIVE"
        const val ACTION_DISCONNECT = "io.github.xiangwen.wifideck.action.DISCONNECT_BLE"

        fun start(context: Context) {
            val intent = Intent(context, BleKeepAliveService::class.java).setAction(ACTION_START)
            try {
                ContextCompat.startForegroundService(context, intent)
            } catch (e: SecurityException) {
                Log.w(TAG, "Cannot start keep-alive service (permission): ${e.message}")
            } catch (e: IllegalStateException) {
                // Covers ForegroundServiceStartNotAllowedException (API 31+) without API-level verify issues
                Log.w(TAG, "Cannot start keep-alive service (${e.javaClass.simpleName}): ${e.message}")
            }
        }

        fun stop(context: Context) {
            val intent = Intent(context, BleKeepAliveService::class.java)
            try {
                context.stopService(intent)
            } catch (e: SecurityException) {
                Log.w(TAG, "Cannot stop keep-alive service: ${e.message}")
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Cannot stop keep-alive service (${e.javaClass.simpleName}): ${e.message}")
            }
        }
    }

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var observerJob: Job? = null
    private var isForegroundStarted = false
    private var notificationManager: NotificationManager? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        BleManager.init(applicationContext)
        notificationManager = getSystemService(NotificationManager::class.java)
        ensureNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_DISCONNECT -> {
                BleManager.disconnect(userInitiated = true)
                stopForegroundAndSelf()
                return START_NOT_STICKY
            }
            ACTION_STOP -> {
                stopForegroundAndSelf()
                return START_NOT_STICKY
            }
        }

        if (!promoteToForeground()) {
            stopSelf()
            return START_NOT_STICKY
        }

        val currentConn = BleManager.connectionState.value
        val currentReconnect = BleManager.reconnectState.value
        if (currentConn == BleManager.ConnectionState.DISCONNECTED && !currentReconnect.isReconnecting) {
            stopForegroundAndSelf()
            return START_NOT_STICKY
        }

        startObservingBleState()
        return START_NOT_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        BleManager.disconnect(userInitiated = true)
        stopForegroundAndSelf()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        observerJob?.cancel()
        observerJob = null
        serviceScope.cancel()
        isForegroundStarted = false
        super.onDestroy()
    }

    private fun startObservingBleState() {
        if (observerJob?.isActive == true) return
        observerJob = serviceScope.launch {
            combine(
                BleManager.connectionState,
                BleManager.reconnectState
            ) { conn, reconnect ->
                conn to reconnect
            }.collect { (conn, reconnect) ->
                if (conn == BleManager.ConnectionState.DISCONNECTED && !reconnect.isReconnecting) {
                    stopForegroundAndSelf()
                } else {
                    updateNotification(conn, reconnect)
                }
            }
        }
    }

    private fun ensureNotificationChannel() {
        val mgr = notificationManager ?: return
        val existing = mgr.getNotificationChannel(CHANNEL_ID)
        if (existing != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.ble_keepalive_channel_name),
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = getString(R.string.ble_keepalive_channel_desc)
            setShowBadge(false)
        }
        mgr.createNotificationChannel(channel)
    }

    private fun promoteToForeground(): Boolean {
        if (isForegroundStarted) {
            updateNotification(BleManager.connectionState.value, BleManager.reconnectState.value)
            return true
        }
        val notification = buildNotification(
            BleManager.connectionState.value,
            BleManager.reconnectState.value
        )
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                ServiceCompat.startForeground(
                    this,
                    NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
                )
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            isForegroundStarted = true
            true
        } catch (e: SecurityException) {
            Log.w(TAG, "startForeground SecurityException: ${e.message}")
            false
        } catch (e: IllegalStateException) {
            Log.w(TAG, "startForeground ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    private fun updateNotification(
        conn: BleManager.ConnectionState,
        reconnect: ReconnectUiState
    ) {
        if (!isForegroundStarted) return
        val mgr = notificationManager ?: return
        try {
            mgr.notify(NOTIFICATION_ID, buildNotification(conn, reconnect))
        } catch (e: SecurityException) {
            Log.w(TAG, "notify SecurityException: ${e.message}")
        }
    }

    private fun buildNotification(
        conn: BleManager.ConnectionState,
        reconnect: ReconnectUiState
    ): Notification {
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val disconnectIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, BleKeepAliveService::class.java).setAction(ACTION_DISCONNECT),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val statusText = when {
            reconnect.isReconnecting -> getString(
                R.string.ble_keepalive_text_reconnecting,
                reconnect.attempt.coerceAtLeast(1),
                reconnect.maxAttempts
            )
            conn == BleManager.ConnectionState.CONNECTED ->
                getString(R.string.ble_keepalive_text_connected)
            else ->
                getString(R.string.ble_keepalive_text_connecting)
        }

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle(getString(R.string.ble_keepalive_title))
            .setContentText(statusText)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .addAction(
                0,
                getString(R.string.ble_keepalive_action_disconnect),
                disconnectIntent
            )
            .build()
    }

    private fun stopForegroundAndSelf() {
        observerJob?.cancel()
        observerJob = null
        if (isForegroundStarted) {
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            isForegroundStarted = false
        }
        stopSelf()
    }
}
