package com.TONE3000.USBCHECK;

/**
 * JNI surface for the probe's native half (probe_jni.cpp), which links
 * plugin/usb (t3k_usb_audio) and the raw usbfs transport. All native logging
 * goes to logcat under the single T3KUSB tag as one-line structured records
 * (plan section 4, L1); the returned strings are for the on-screen log and
 * the CSV/fixture files the Java side writes.
 */
public final class NativeBridge {
    static {
        System.loadLibrary("t3k_probe");
    }

    private NativeBridge() {}

    /** Kernel driver bound to an interface before we claim it (USBDEVFS_GETDRIVER);
        empty string if none. */
    public static native String nativeGetDriver(int fd, int interfaceNumber);

    /** Quirk registry: true = leave this interface unclaimed (plan M2.5). */
    public static native boolean nativeQuirkIgnoresInterface(int vid, int pid, int interfaceNumber);

    /** Parse the raw descriptor blob and return the human-readable tree. */
    public static native String nativeDump(byte[] rawDescriptors, int vid, int pid);

    /** Open the stream (rate/clock control + SET_INTERFACE + URB machine) on
        a background USB thread. Returns an empty string on success. */
    public static native String nativeStart(int fd, int vid, int pid, int rate,
                                            int channels, int subslot, int packetsPerUrb,
                                            int urbsInFlight);

    /** Stop the stream cleanly (DISCARDURB, SET_INTERFACE 0). */
    public static native String nativeStop();

    /** Self-measure round-trip latency (plan section 4.1) and return the CSV
        rows (one per trial) plus a summary line. */
    public static native String nativeRtl(int fd, int vid, int pid, int rate,
                                          int channels, int subslot, int packetsPerUrb,
                                          int urbsInFlight, int trials);

    /** Sweep rtl across the frames/URB x URBs-in-flight matrix (plan M1.6)
        and return one CSV row per configuration. */
    public static native String nativeSweep(int fd, int vid, int pid, int rate,
                                            int channels, int subslot);
}
