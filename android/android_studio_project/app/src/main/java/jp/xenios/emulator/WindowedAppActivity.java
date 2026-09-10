package jp.xenios.emulator;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;

import org.jetbrains.annotations.Nullable;

import jp.xenios.XeniosRuntimeException;

public abstract class WindowedAppActivity extends Activity {
    // The EXTRA_CVARS value literal is also used in the native code.

    /**
     * Name of the Bundle intent extra containing XeniOS config variable launch arguments.
     */
    public static final String EXTRA_CVARS = "jp.xenios.emulator.WindowedAppActivity.EXTRA_CVARS";

    static {
        System.loadLibrary("xenia-app");
    }

    private final WindowSurfaceListener mWindowSurfaceListener = new WindowSurfaceListener();

    // May be 0 while destroying (mainly while the superclass is).
    private long mAppContext = 0;
    private volatile boolean mWindowPaintingSuspended;

    @Nullable
    private WindowSurfaceView mWindowSurfaceView = null;

    private native long initializeWindowedAppOnCreate(
            String windowedAppIdentifier, AssetManager assetManager);

    private native void onDestroyNative(long appContext);

    private native void onTrimMemoryNative(long appContext, int level);

    private native void onWindowSurfaceLayoutChange(
            long appContext, int left, int top, int right, int bottom);

    private native boolean onWindowSurfaceMotionEvent(long appContext, MotionEvent event);

    private native void onWindowSurfaceChanged(long appContext, Surface windowSurface);

    private native void paintWindow(long appContext, boolean forcePaint);

    private native String collectNativeDiagnosticSnapshotNative(long appContext);
    private native String runDiagnosticSaveStateNative(
            long appContext, String path, boolean restore);

    protected abstract String getWindowedAppIdentifier();

    protected boolean isWindowedAppReady() {
        return mAppContext != 0;
    }

    protected final String collectNativeDiagnosticSnapshot() {
        if (mAppContext == 0) {
            return "{}";
        }
        final String snapshot = collectNativeDiagnosticSnapshotNative(mAppContext);
        return snapshot != null ? snapshot : "{}";
    }

    protected final String runDiagnosticSaveState(
            final String path, final boolean restore) {
        if (mAppContext == 0) {
            return "error\tThe emulator runtime is unavailable.";
        }
        final String result =
                runDiagnosticSaveStateNative(mAppContext, path, restore);
        return result != null ? result : "error\tThe native operation failed.";
    }

    private void enterImmersiveMode() {
        getWindow().setFlags(
                WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    protected void setWindowSurfaceView(@Nullable final WindowSurfaceView windowSurfaceView) {
        if (mWindowSurfaceView == windowSurfaceView) {
            return;
        }

        // Detach from the old surface.
        if (mWindowSurfaceView != null) {
            mWindowSurfaceView.getHolder().removeCallback(mWindowSurfaceListener);
            mWindowSurfaceView.setOnTouchListener(null);
            mWindowSurfaceView.setOnGenericMotionListener(null);
            mWindowSurfaceView.removeOnLayoutChangeListener(mWindowSurfaceListener);
            mWindowSurfaceView = null;
            if (mAppContext != 0) {
                onWindowSurfaceChanged(mAppContext, null);
            }
        }

        if (windowSurfaceView == null) {
            return;
        }

        mWindowSurfaceView = windowSurfaceView;
        // FIXME(Triang3l): This doesn't work if the layout has already been performed.
        mWindowSurfaceView.addOnLayoutChangeListener(mWindowSurfaceListener);
        mWindowSurfaceView.setOnGenericMotionListener(mWindowSurfaceListener);
        mWindowSurfaceView.setOnTouchListener(mWindowSurfaceListener);
        final SurfaceHolder windowSurfaceHolder = mWindowSurfaceView.getHolder();
        windowSurfaceHolder.addCallback(mWindowSurfaceListener);
        // If setting after the creation of the surface.
        if (mAppContext != 0) {
            final Surface windowSurface = windowSurfaceHolder.getSurface();
            if (windowSurface != null) {
                onWindowSurfaceChanged(mAppContext, windowSurface);
            }
        }
    }

    public void onWindowSurfaceDraw(final boolean forcePaint) {
        if (mAppContext == 0 || mWindowPaintingSuspended) {
            return;
        }
        paintWindow(mAppContext, forcePaint);
    }

    protected final void setWindowPaintingSuspended(boolean suspended) {
        mWindowPaintingSuspended = suspended;
        if (!suspended) {
            postInvalidateWindowSurface();
        }
    }

    // Used from the native WindowedAppContext. May be called from non-UI threads.
    @SuppressWarnings("UnusedDeclaration")
    protected void postInvalidateWindowSurface() {
        if (mWindowSurfaceView == null) {
            return;
        }
        mWindowSurfaceView.postInvalidate();
    }

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        enterImmersiveMode();

        final String windowedAppIdentifier = getWindowedAppIdentifier();
        mAppContext = initializeWindowedAppOnCreate(windowedAppIdentifier, getAssets());
        if (mAppContext == 0) {
            Toast.makeText(
                    this,
                    "Unable to start " + windowedAppIdentifier + ".",
                    Toast.LENGTH_LONG).show();
            finish();
        }
    }

    @Override
    public void onWindowFocusChanged(final boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enterImmersiveMode();
        }
    }

    @Override
    public void onTrimMemory(final int level) {
        super.onTrimMemory(level);
        if (mAppContext != 0) {
            onTrimMemoryNative(mAppContext, level);
        }
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        if (mAppContext != 0) {
            onTrimMemoryNative(mAppContext, 80);
        }
    }

    @Override
    protected void onDestroy() {
        setWindowSurfaceView(null);
        if (mAppContext != 0) {
            onDestroyNative(mAppContext);
        }
        mAppContext = 0;
        super.onDestroy();
    }

    private class WindowSurfaceListener implements
            View.OnGenericMotionListener,
            View.OnLayoutChangeListener,
            View.OnTouchListener,
            SurfaceHolder.Callback2 {
        @Override
        public void onLayoutChange(
                final View v, final int left, final int top, final int right, final int bottom,
                final int oldLeft, final int oldTop, final int oldRight, final int oldBottom) {
            if (mAppContext != 0) {
                onWindowSurfaceLayoutChange(mAppContext, left, top, right, bottom);
            }
        }

        @Override
        public boolean onGenericMotion(final View view, final MotionEvent event) {
            if (mAppContext == 0) {
                return false;
            }
            return onWindowSurfaceMotionEvent(mAppContext, event);
        }

        @SuppressLint("ClickableViewAccessibility")
        @Override
        public boolean onTouch(final View view, final MotionEvent event) {
            if (mAppContext == 0) {
                return false;
            }
            return onWindowSurfaceMotionEvent(mAppContext, event);
        }

        @Override
        public void surfaceCreated(final SurfaceHolder holder) {
            if (mAppContext == 0) {
                return;
            }
            onWindowSurfaceChanged(mAppContext, holder.getSurface());
        }

        @Override
        public void surfaceChanged(
                final SurfaceHolder holder, final int format, final int width, final int height) {
            if (mAppContext == 0) {
                return;
            }
            onWindowSurfaceChanged(mAppContext, holder.getSurface());
        }

        @Override
        public void surfaceDestroyed(final SurfaceHolder holder) {
            if (mAppContext == 0) {
                return;
            }
            onWindowSurfaceChanged(mAppContext, null);
        }

        @Override
        public void surfaceRedrawNeeded(final SurfaceHolder holder) {
            onWindowSurfaceDraw(true);
        }
    }
}
