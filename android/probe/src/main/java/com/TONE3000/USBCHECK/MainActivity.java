package com.TONE3000.USBCHECK;

import android.app.Activity;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * Probe UI (plan section 4, L1): one button per mode, a scrolling log, and a
 * "mode" intent extra so the iteration loop can drive everything over adb
 * (script/android-dev.sh). Modes: dump (descriptor tree + fixtures), stream
 * (sustained duplex with xrun/URB stats), rtl (self-measurement, section
 * 4.1), sweep (rtl across the URB layout matrix, plan M1.6).
 */
public final class MainActivity extends Activity implements UsbShell.Listener {
    private static final String TAG = UsbShell.TAG;

    private UsbShell shell;
    private TextView logView;
    private boolean streaming = false;
    private int activeFd = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(modeButton("dump", "Dump"));
        buttons.addView(modeButton("stream", "Stream"));
        buttons.addView(modeButton("rtl", "RTL"));
        buttons.addView(modeButton("sweep", "Sweep"));
        buttons.addView(modeButton("stop", "Stop"));
        root.addView(buttons);

        logView = new TextView(this);
        logView.setGravity(Gravity.BOTTOM);
        logView.setTextSize(11);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(logView);
        root.addView(scroll);
        setContentView(root);

        shell = new UsbShell(this, this);
        shell.start();

        String mode = getIntent().getStringExtra("mode");
        if (mode != null) {
            log("mode: " + mode + " (from intent)");
            runMode(mode);
        } else {
            log("ready: tap a mode (or am start -e mode dump)");
        }
    }

    @Override
    protected void onDestroy() {
        NativeBridge.nativeStop();
        shell.stop();
        super.onDestroy();
    }

    private Button modeButton(final String mode, String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(v -> runMode(mode));
        return b;
    }

    private void runMode(String mode) {
        UsbDevice device = shell.getActiveDevice();
        if (!"stop".equals(mode) && device == null) {
            log("no device yet: enumerating");
            shell.findAudioDevice();
            pendingMode = mode;
            return;
        }
        switch (mode) {
            case "dump":
                runDump();
                break;
            case "stream":
                runStream();
                break;
            case "rtl":
                runRtl();
                break;
            case "sweep":
                runSweep();
                break;
            case "stop":
                log("stop: " + NativeBridge.nativeStop());
                streaming = false;
                break;
            default:
                log("unknown mode: " + mode);
                break;
        }
    }

    private String pendingMode = null;

    private void runDump() {
        byte[] raw = shell.getRawDescriptors();
        UsbDevice device = shell.getActiveDevice();
        if (raw == null || device == null) {
            log("dump: no raw descriptors (device not open)");
            return;
        }
        // Save the fixture for the L0 parser tests (plan M1.3).
        String name = "ams22-descriptors.bin";
        try (FileOutputStream out = openFileOutput(name, MODE_PRIVATE)) {
            out.write(raw);
            log("dump: wrote files/" + name + " (" + raw.length + " bytes)");
        } catch (IOException e) {
            log("dump: fixture write failed: " + e.getMessage());
        }
        String tree = NativeBridge.nativeDump(raw, device.getVendorId(), device.getProductId());
        for (String line : tree.split("\n")) log(line);
    }

    private void runStream() {
        UsbDevice device = shell.getActiveDevice();
        if (device == null) return;
        String err = NativeBridge.nativeStart(
                activeFd, device.getVendorId(), device.getProductId(),
                48000, 2, 3, 8, 3);
        log("stream: " + (err.isEmpty() ? "running (48 kHz/2ch/subslot 3, 3x8)" : err));
        streaming = err.isEmpty();
    }

    private void runRtl() {
        UsbDevice device = shell.getActiveDevice();
        if (device == null) return;
        String rows = NativeBridge.nativeRtl(
                activeFd, device.getVendorId(), device.getProductId(),
                48000, 2, 3, 8, 3, 5);
        appendCsv(rows);
        for (String line : rows.split("\n")) log(line);
    }

    private void runSweep() {
        UsbDevice device = shell.getActiveDevice();
        if (device == null) return;
        String rows = NativeBridge.nativeSweep(
                activeFd, device.getVendorId(), device.getProductId(), 48000, 2, 3);
        appendCsv(rows);
        for (String line : rows.split("\n")) log(line);
    }

    private void appendCsv(String rows) {
        if (rows == null || rows.isEmpty()) return;
        try (FileOutputStream out = openFileOutput("rtl.csv", MODE_APPEND)) {
            out.write((rows + "\n").getBytes(StandardCharsets.UTF_8));
            log("csv: appended to files/rtl.csv");
        } catch (IOException e) {
            log("csv: write failed: " + e.getMessage());
        }
    }

    private void log(String line) {
        Log.i(TAG, line);
        logView.append(line + "\n");
    }

    @Override
    public void onLog(String line) {
        log(line);
    }

    @Override
    public void onDeviceReady(UsbDevice device, UsbDeviceConnection connection, int fd) {
        activeFd = fd;
        log("device ready: fd=" + fd);
        if (pendingMode != null) {
            String mode = pendingMode;
            pendingMode = null;
            runMode(mode);
        }
    }

    @Override
    public void onDeviceDetached() {
        streaming = false;
        log("stream stopped: device detached");
    }
}
