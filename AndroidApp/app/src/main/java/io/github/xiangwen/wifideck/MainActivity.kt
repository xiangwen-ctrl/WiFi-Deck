package io.github.xiangwen.wifideck

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import io.github.xiangwen.wifideck.data.BleGatePrompt
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.ui.BleGateAction
import io.github.xiangwen.wifideck.ui.MainScreen
import io.github.xiangwen.wifideck.ui.MainViewModel
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    companion object {
        private const val PREFS_NAME = "wifideck_ble_gate_prefs"
        private const val KEY_BLE_PERMS_PERMANENTLY_DENIED = "ble_perms_permanently_denied"
    }

    private val viewModel: MainViewModel by viewModels()

    private val gatePrefs by lazy {
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    }

    private var pendingResumeGateCheck = false
    private var redirectedFromPrompt: BleGatePrompt? = null
    private var btStateReceiverRegistered = false

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { _ ->
        val missing = missingBlePermissions()
        if (missing.isEmpty()) {
            setPermanentlyDenied(false)
            runBlePreFlightGate(autoTriggered = false)
        } else {
            val isAndroid12Plus = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
            val canAskAgain = missing.any {
                ActivityCompat.shouldShowRequestPermissionRationale(this, it)
            }
            if (canAskAgain) {
                setPermanentlyDenied(false)
                viewModel.showToast("需要蓝牙/定位权限才能扫描并连接 WiFi-Deck")
            } else {
                setPermanentlyDenied(true)
                viewModel.showBleGatePrompt(BleGatePrompt.PermissionPermanentlyDenied(isAndroid12Plus))
            }
        }
    }

    private val enableBluetoothLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { _ ->
        if (BleManager.isBluetoothEnabled()) {
            viewModel.dismissBleGatePrompt()
            runBlePreFlightGate(autoTriggered = false)
        } else {
            viewModel.showToast("蓝牙未开启，无法扫描 WiFi-Deck")
        }
    }

    private val btStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            val state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)
            when (state) {
                BluetoothAdapter.STATE_TURNING_OFF,
                BluetoothAdapter.STATE_OFF -> viewModel.onBluetoothStateChanged(enabled = false)
                BluetoothAdapter.STATE_ON -> viewModel.onBluetoothStateChanged(enabled = true)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.parseColor("#11141C")),
            navigationBarStyle = SystemBarStyle.dark(Color.parseColor("#11141C"))
        )
        super.onCreate(savedInstanceState)
        BleManager.init(this)
        registerBluetoothStateReceiver()

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.gateActions.collect { action ->
                    handleGateAction(action)
                }
            }
        }

        setContent {
            MainScreen(viewModel)
        }

        // 仅在首次冷启动且处于未连接态时触发自动扫描，旋转/深色模式/分屏重建绝不强断已有连接
        if (savedInstanceState == null &&
            BleManager.connectionState.value == BleManager.ConnectionState.DISCONNECTED
        ) {
            viewModel.requestBleConnect(autoTriggered = true)
        }
    }

    override fun onResume() {
        super.onResume()
        if (pendingResumeGateCheck) {
            pendingResumeGateCheck = false
            val fromPrompt = redirectedFromPrompt
            redirectedFromPrompt = null
            runBlePreFlightGate(autoTriggered = false, returnedFromPrompt = fromPrompt)
        }
    }

    override fun onDestroy() {
        if (btStateReceiverRegistered) {
            try {
                unregisterReceiver(btStateReceiver)
            } catch (_: IllegalArgumentException) {
            }
            btStateReceiverRegistered = false
        }
        super.onDestroy()
    }

    private fun registerBluetoothStateReceiver() {
        if (btStateReceiverRegistered) return
        ContextCompat.registerReceiver(
            this,
            btStateReceiver,
            IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED),
            ContextCompat.RECEIVER_EXPORTED
        )
        btStateReceiverRegistered = true
    }

    private fun handleGateAction(action: BleGateAction) {
        when (action) {
            is BleGateAction.CheckAndConnect -> runBlePreFlightGate(action.autoTriggered)
            BleGateAction.LaunchPermissionRequest -> {
                val missing = missingBlePermissions()
                if (missing.isEmpty()) {
                    setPermanentlyDenied(false)
                    runBlePreFlightGate(autoTriggered = false)
                } else {
                    permissionLauncher.launch(missing.toTypedArray())
                }
            }
            BleGateAction.LaunchAppSettings -> launchAppSettings()
            BleGateAction.LaunchEnableBluetooth -> launchEnableBluetooth()
            BleGateAction.LaunchLocationSettings -> launchLocationSettings()
        }
    }

    private fun requiredBlePermissions(): List<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            listOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            listOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    private fun missingBlePermissions(): List<String> {
        return requiredBlePermissions().filter { perm ->
            ContextCompat.checkSelfPermission(this, perm) != PackageManager.PERMISSION_GRANTED
        }
    }

    private fun isPermanentlyDenied(): Boolean {
        return gatePrefs.getBoolean(KEY_BLE_PERMS_PERMANENTLY_DENIED, false)
    }

    private fun setPermanentlyDenied(value: Boolean) {
        gatePrefs.edit().putBoolean(KEY_BLE_PERMS_PERMANENTLY_DENIED, value).apply()
    }

    private fun runBlePreFlightGate(
        autoTriggered: Boolean,
        returnedFromPrompt: BleGatePrompt? = null
    ) {
        if (BleManager.connectionState.value != BleManager.ConnectionState.DISCONNECTED) {
            return
        }
        if (!BleManager.isDeviceSupported()) {
            viewModel.showToast("当前设备不支持 BLE 或蓝牙不可用")
            return
        }

        // 1. 运行时权限门禁（Android 12+ 需 BLUETOOTH_SCAN/CONNECT；Android 8~11 需 ACCESS_FINE_LOCATION）
        val missing = missingBlePermissions()
        if (missing.isNotEmpty()) {
            val isAndroid12Plus = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
            val shouldShowRationale = missing.any {
                ActivityCompat.shouldShowRequestPermissionRationale(this, it)
            }
            when {
                shouldShowRationale -> {
                    setPermanentlyDenied(false)
                    viewModel.showBleGatePrompt(BleGatePrompt.PermissionRationale(isAndroid12Plus))
                }
                isPermanentlyDenied() -> {
                    if (returnedFromPrompt is BleGatePrompt.PermissionPermanentlyDenied || autoTriggered) {
                        viewModel.showToast("缺少蓝牙/定位权限，请点击「连接设备」前往设置授权")
                    } else {
                        viewModel.showBleGatePrompt(
                            BleGatePrompt.PermissionPermanentlyDenied(isAndroid12Plus)
                        )
                    }
                }
                else -> {
                    permissionLauncher.launch(missing.toTypedArray())
                }
            }
            return
        }
        setPermanentlyDenied(false)

        // 2. 蓝牙开关门禁（权限就绪后再检查蓝牙，避免无 BLUETOOTH_CONNECT 调起系统弹框抛 SecurityException）
        if (!BleManager.isBluetoothEnabled()) {
            if (returnedFromPrompt == BleGatePrompt.BluetoothDisabled) {
                viewModel.showToast("蓝牙仍未开启，请开启后点击「连接设备」")
            } else {
                viewModel.showBleGatePrompt(BleGatePrompt.BluetoothDisabled)
            }
            return
        }

        // 3. Android 8~11 (API < 31) 系统定位总开关门禁
        if (!BleManager.isLocationReadyForScan()) {
            if (returnedFromPrompt == BleGatePrompt.LocationDisabled) {
                viewModel.showToast("系统定位仍未开启，开启后才可扫描附近蓝牙设备")
            } else {
                viewModel.showBleGatePrompt(BleGatePrompt.LocationDisabled)
            }
            return
        }

        viewModel.dismissBleGatePrompt()
        BleManager.startScan()
    }

    private fun launchAppSettings() {
        pendingResumeGateCheck = true
        redirectedFromPrompt = BleGatePrompt.PermissionPermanentlyDenied(
            isAndroid12Plus = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
        )
        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.fromParts("package", packageName, null)
        }
        try {
            startActivity(intent)
        } catch (_: ActivityNotFoundException) {
            try {
                startActivity(Intent(Settings.ACTION_SETTINGS))
            } catch (_: ActivityNotFoundException) {
                pendingResumeGateCheck = false
                redirectedFromPrompt = null
                viewModel.showToast("无法打开系统设置，请手动前往设置授权")
            }
        }
    }

    private fun launchLocationSettings() {
        pendingResumeGateCheck = true
        redirectedFromPrompt = BleGatePrompt.LocationDisabled
        try {
            startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
        } catch (_: ActivityNotFoundException) {
            try {
                startActivity(Intent(Settings.ACTION_SETTINGS))
            } catch (_: ActivityNotFoundException) {
                pendingResumeGateCheck = false
                redirectedFromPrompt = null
                viewModel.showToast("无法打开定位设置，请手动开启系统定位")
            }
        }
    }

    private fun launchEnableBluetooth() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
            != PackageManager.PERMISSION_GRANTED
        ) {
            runBlePreFlightGate(autoTriggered = false)
            return
        }
        try {
            enableBluetoothLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        } catch (_: SecurityException) {
            runBlePreFlightGate(autoTriggered = false)
        } catch (_: ActivityNotFoundException) {
            pendingResumeGateCheck = true
            redirectedFromPrompt = BleGatePrompt.BluetoothDisabled
            try {
                startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
            } catch (_: ActivityNotFoundException) {
                pendingResumeGateCheck = false
                redirectedFromPrompt = null
                viewModel.showToast("请手动前往系统设置开启蓝牙")
            }
        }
    }
}
