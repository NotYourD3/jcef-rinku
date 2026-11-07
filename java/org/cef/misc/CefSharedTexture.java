package org.cef.misc;

import java.nio.ByteBuffer;

public final class CefSharedTexture {
    private CefSharedTexture() {}

    public static native boolean isSupported();

    public static native boolean initWglInterop();

    /**
     * Must be called synchronously from inside CefRenderHandler.onAcceleratedPaint. CEF returns
     * the shared texture to its pool as soon as the callback returns, so the handle is invalid
     * afterwards. This reopens the handle and performs a VRAM-to-VRAM copy into a texture owned
     * by this process; no BGRA pixels cross the CPU.
     */
    public static native boolean copySharedTextureFrame(long sharedTextureHandle, int width, int height);

    /**
     * Fallback for drivers without WGL_NV_DX_interop. Also only valid inside the callback.
     */
    public static native boolean readPixels(long sharedTextureHandle, int width, int height, ByteBuffer output);

    /**
     * Called on the GL render thread. Publishes the newest frame to the persistent GL texture and
     * locks it for sampling. The handle argument is retained for compatibility and is ignored by
     * the native side (it is already invalid at this point).
     */
    public static native int lockSharedTexture(long sharedTextureHandle, int width, int height);

    /**
     * Actual pixel size of the GL texture backing the frame. CEF allocates it at viewport *
     * device_scale_factor, so it is usually larger than the logical viewport passed to
     * lockSharedTexture(). High 32 bits = width, low 32 bits = height.
     */
    public static native long getFrameSize();

    public static native void unlockSharedTexture();

    /** Human-readable interop state, for diagnosing why the zero-copy path is unavailable. */
    public static native String getDiagnostics();

    public static native void cleanup();
}
