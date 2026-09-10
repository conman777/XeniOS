package jp.xenios.emulator;

import android.app.AlertDialog;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

public class EmulatorActivity extends WindowedAppActivity {
    private static final int XINPUT_DPAD_UP = 0x0001;
    private static final int XINPUT_DPAD_DOWN = 0x0002;
    private static final int XINPUT_DPAD_LEFT = 0x0004;
    private static final int XINPUT_DPAD_RIGHT = 0x0008;
    private static final int XINPUT_START = 0x0010;
    private static final int XINPUT_BACK = 0x0020;
    private static final int XINPUT_LEFT_THUMB = 0x0040;
    private static final int XINPUT_RIGHT_THUMB = 0x0080;
    private static final int XINPUT_LEFT_SHOULDER = 0x0100;
    private static final int XINPUT_RIGHT_SHOULDER = 0x0200;
    private static final int XINPUT_GUIDE = 0x0400;
    private static final int XINPUT_A = 0x1000;
    private static final int XINPUT_B = 0x2000;
    private static final int XINPUT_X = 0x4000;
    private static final int XINPUT_Y = 0x8000;

    private int mGamepadKeyButtons;
    private int mGamepadHatButtons;
    private boolean mGamepadLeftTriggerDigital;
    private boolean mGamepadRightTriggerDigital;
    private float mGamepadLeftX;
    private float mGamepadLeftY;
    private float mGamepadRightX;
    private float mGamepadRightY;
    private float mGamepadLeftTrigger;
    private float mGamepadRightTrigger;
    private long mDiagnosticOperationGeneration;

    private native void setGamepadStateNative(
            int buttons, float leftX, float leftY, float rightX, float rightY,
            float leftTrigger, float rightTrigger);

    private native void resetGamepadStateNative();

    @Override
    protected String getWindowedAppIdentifier() {
        return "xenia";
    }

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        // Debug builds may be launched directly by adb / automated device
        // tests with a plain string "target" extra. Production launches still
        // use the complete cvar Bundle created by LauncherActivity.
        if (BuildConfig.DEBUG
                && getIntent().getBundleExtra(WindowedAppActivity.EXTRA_CVARS) == null) {
            final String directTarget = getIntent().getStringExtra("target");
            if (directTarget != null && !directTarget.isEmpty()) {
                final Bundle launchArguments = new Bundle();
                final java.io.File contentRoot =
                        new java.io.File(getFilesDir(), "content");
                contentRoot.mkdirs();
                launchArguments.putString("target", directTarget);
                launchArguments.putString(
                        "storage_root", getFilesDir().getAbsolutePath());
                launchArguments.putString(
                        "content_root", contentRoot.getAbsolutePath());
                launchArguments.putString(
                        "cache_root", getCacheDir().getAbsolutePath());
                launchArguments.putString("apu", "opensl");
                launchArguments.putString("gpu", "vulkan");
                launchArguments.putString("hid", "nop");
                launchArguments.putBoolean("discord", false);
                launchArguments.putBoolean(
                        "a64_fail_fast_on_access_violation", false);
                launchArguments.putBoolean("log_undefined_extern_args", true);
                getIntent().putExtra(
                        WindowedAppActivity.EXTRA_CVARS, launchArguments);
            }
        }
        super.onCreate(savedInstanceState);
        if (!isWindowedAppReady()) {
            return;
        }

        setContentView(R.layout.activity_emulator);
        setWindowSurfaceView(findViewById(R.id.emulator_surface_view));
        configureDebugDiagnostics();
    }

    private void configureDebugDiagnostics() {
        final View panel = findViewById(R.id.debug_diagnostics_panel);
        final Button toggle = findViewById(R.id.debug_diagnostics_toggle);
        if (!BuildConfig.DEBUG) {
            panel.setVisibility(View.GONE);
            toggle.setVisibility(View.GONE);
            return;
        }

        panel.setVisibility(View.GONE);
        toggle.setVisibility(View.VISIBLE);
        toggle.setOnClickListener(view -> {
            final boolean show = panel.getVisibility() != View.VISIBLE;
            panel.setVisibility(show ? View.VISIBLE : View.GONE);
            toggle.setText(show ? R.string.debug_tools_close : R.string.debug_tools_open);
        });
        final Button reportButton = findViewById(R.id.report_diagnostics_button);
        final Button captureButton = findViewById(R.id.capture_diagnostics_button);
        final Button restoreButton = findViewById(R.id.restore_diagnostic_save_button);
        final TextView statusView = findViewById(R.id.debug_diagnostics_status);
        reportButton.setOnClickListener(view -> {
            setDiagnosticControlsEnabled(reportButton, captureButton, restoreButton, false);
            statusView.setText(R.string.debug_diagnostics_report_collecting);

            final String nativeSnapshot = collectNativeDiagnosticSnapshot();
            final String targetPath = getDiagnosticTargetPath();
            new Thread(() -> {
                final DiagnosticCaptureManager.Result result =
                        DiagnosticCaptureManager.captureReport(
                                getApplicationContext(), targetPath, nativeSnapshot);
                runOnUiThread(() -> finishDiagnosticOperation(
                        reportButton, captureButton, restoreButton, statusView,
                        result.isSuccess(),
                        result.isSuccess()
                                ? getString(
                                        R.string.debug_diagnostics_report_created,
                                        result.getRequestId())
                                : result.getErrorMessage(),
                        false));
            }, "XeniOS diagnostic report").start();
        });

        captureButton.setOnClickListener(view -> {
            setDiagnosticControlsEnabled(reportButton, captureButton, restoreButton, false);
            setWindowPaintingSuspended(true);
            beginLongDiagnosticOperation(
                    statusView, R.string.debug_diagnostics_collecting);

            final String nativeSnapshot = collectNativeDiagnosticSnapshot();
            final String targetPath = getDiagnosticTargetPath();
            new Thread(() -> {
                final String saveStatePath = getDiagnosticSaveStatePath();
                final String saveResult =
                        runDiagnosticSaveState(saveStatePath, false);
                if (!isNativeOperationSuccessful(saveResult)) {
                    runOnUiThread(() -> finishDiagnosticOperation(
                            reportButton, captureButton, restoreButton, statusView, false,
                            nativeOperationMessage(saveResult), true));
                    return;
                }
                final DiagnosticCaptureManager.Result result =
                        DiagnosticCaptureManager.capture(
                                getApplicationContext(), targetPath, nativeSnapshot,
                                saveStatePath);
                runOnUiThread(() -> finishDiagnosticOperation(
                        reportButton, captureButton, restoreButton, statusView,
                        result.isSuccess(),
                        result.isSuccess()
                                ? getString(
                                        R.string.debug_diagnostics_created,
                                        result.getRequestId())
                                : result.getErrorMessage(),
                        true));
            }, "XeniOS diagnostic capture").start();
        });

        restoreButton.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle(R.string.debug_diagnostics_restore_title)
                .setMessage(R.string.debug_diagnostics_restore_confirmation)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                    setDiagnosticControlsEnabled(
                            reportButton, captureButton, restoreButton, false);
                    setWindowPaintingSuspended(true);
                    beginLongDiagnosticOperation(
                            statusView, R.string.debug_diagnostics_restoring);
                    new Thread(() -> {
                        final String restoreResult = runDiagnosticSaveState(
                                getDiagnosticSaveStatePath(), true);
                        runOnUiThread(() -> {
                            final boolean success =
                                    isNativeOperationSuccessful(restoreResult);
                            finishDiagnosticOperation(
                                    reportButton, captureButton, restoreButton,
                                    statusView, success,
                                    success
                                            ? getString(R.string.debug_diagnostics_restored)
                                            : nativeOperationMessage(restoreResult),
                                    true);
                        });
                    }, "XeniOS diagnostic restore").start();
                })
                .show());
    }

    private String getDiagnosticSaveStatePath() {
        final java.io.File externalRoot = getExternalFilesDir(null);
        final java.io.File saveDirectory =
                new java.io.File(externalRoot != null ? externalRoot : getFilesDir(),
                        "diagnostic_save");
        if (!saveDirectory.isDirectory()) {
            saveDirectory.mkdirs();
        }
        return new java.io.File(saveDirectory, "current.xes").getAbsolutePath();
    }

    private static boolean isNativeOperationSuccessful(final String result) {
        return result != null && result.startsWith("ok\t");
    }

    private static String nativeOperationMessage(final String result) {
        if (result == null) {
            return "Native operation returned no result.";
        }
        final int separator = result.indexOf('\t');
        return separator >= 0 ? result.substring(separator + 1) : result;
    }

    private void finishDiagnosticOperation(
            final Button reportButton, final Button captureButton,
            final Button restoreButton, final TextView statusView,
            final boolean success, final String message,
            final boolean resumePainting) {
        if (resumePainting) {
            ++mDiagnosticOperationGeneration;
            setWindowPaintingSuspended(false);
        }
        setDiagnosticControlsEnabled(reportButton, captureButton, restoreButton, true);
        final String displayMessage = success
                ? message
                : getString(R.string.debug_diagnostics_failed, message);
        statusView.setText(displayMessage);
        Toast.makeText(this, displayMessage, Toast.LENGTH_LONG).show();
    }

    private void beginLongDiagnosticOperation(
            final TextView statusView, final int messageResource) {
        final long generation = ++mDiagnosticOperationGeneration;
        final long startedAt = SystemClock.elapsedRealtime();
        statusView.post(new Runnable() {
            @Override
            public void run() {
                if (generation != mDiagnosticOperationGeneration) {
                    return;
                }
                final long elapsedSeconds =
                        (SystemClock.elapsedRealtime() - startedAt) / 1000L;
                statusView.setText(getString(messageResource, elapsedSeconds));
                statusView.postDelayed(this, 1000L);
            }
        });
    }

    private static void setDiagnosticControlsEnabled(
            final Button reportButton, final Button captureButton,
            final Button restoreButton, final boolean enabled) {
        reportButton.setEnabled(enabled);
        captureButton.setEnabled(enabled);
        restoreButton.setEnabled(enabled);
    }

    private String getDiagnosticTargetPath() {
        final Bundle launchArguments =
                getIntent().getBundleExtra(WindowedAppActivity.EXTRA_CVARS);
        return launchArguments != null ? launchArguments.getString("target", "") : "";
    }

    private static boolean isGamepadEvent(final KeyEvent event) {
        return event.isFromSource(InputDevice.SOURCE_GAMEPAD)
                || event.isFromSource(InputDevice.SOURCE_JOYSTICK);
    }

    private static boolean isDedicatedGamepadKey(final int keyCode) {
        return keyCode >= KeyEvent.KEYCODE_BUTTON_A
                && keyCode <= KeyEvent.KEYCODE_BUTTON_MODE;
    }

    private static boolean isGamepadEvent(final MotionEvent event) {
        return event.isFromSource(InputDevice.SOURCE_GAMEPAD)
                || event.isFromSource(InputDevice.SOURCE_JOYSTICK);
    }

    private static int xInputButtonForKeyCode(final int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
                return XINPUT_A;
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_ESCAPE:
                return XINPUT_B;
            case KeyEvent.KEYCODE_BUTTON_X:
                return XINPUT_X;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return XINPUT_Y;
            case KeyEvent.KEYCODE_DPAD_UP:
                return XINPUT_DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return XINPUT_DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return XINPUT_DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return XINPUT_DPAD_RIGHT;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_MENU:
                return XINPUT_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return XINPUT_BACK;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return XINPUT_LEFT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return XINPUT_RIGHT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return XINPUT_LEFT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return XINPUT_RIGHT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_MODE:
                return XINPUT_GUIDE;
            default:
                return 0;
        }
    }

    private static boolean hasAxis(final MotionEvent event, final int axis) {
        final InputDevice device = event.getDevice();
        return device != null && device.getMotionRange(axis, event.getSource()) != null;
    }

    private static float getStickAxis(final MotionEvent event, final int axis) {
        final InputDevice device = event.getDevice();
        if (device == null) {
            return 0.0f;
        }
        final InputDevice.MotionRange range = device.getMotionRange(axis, event.getSource());
        if (range == null) {
            return 0.0f;
        }
        final float value = event.getAxisValue(axis);
        if (value >= 0.0f) {
            return range.getMax() > 0.0f ? value / range.getMax() : value;
        }
        return range.getMin() < 0.0f ? value / -range.getMin() : value;
    }

    private static float getTriggerAxis(final MotionEvent event, final int axis) {
        final InputDevice device = event.getDevice();
        if (device == null) {
            return 0.0f;
        }
        final InputDevice.MotionRange range = device.getMotionRange(axis, event.getSource());
        if (range == null) {
            return 0.0f;
        }
        final float minimum = range.getMin();
        final float maximum = range.getMax();
        if (maximum <= minimum) {
            return 0.0f;
        }
        final float normalized = (event.getAxisValue(axis) - minimum) / (maximum - minimum);
        return Math.max(0.0f, Math.min(1.0f, normalized));
    }

    private void pushGamepadState() {
        setGamepadStateNative(
                mGamepadKeyButtons | mGamepadHatButtons,
                mGamepadLeftX, mGamepadLeftY, mGamepadRightX, mGamepadRightY,
                Math.max(mGamepadLeftTrigger, mGamepadLeftTriggerDigital ? 1.0f : 0.0f),
                Math.max(mGamepadRightTrigger, mGamepadRightTriggerDigital ? 1.0f : 0.0f));
    }

    private void clearGamepadState() {
        mGamepadKeyButtons = 0;
        mGamepadHatButtons = 0;
        mGamepadLeftTriggerDigital = false;
        mGamepadRightTriggerDigital = false;
        mGamepadLeftX = 0.0f;
        mGamepadLeftY = 0.0f;
        mGamepadRightX = 0.0f;
        mGamepadRightY = 0.0f;
        mGamepadLeftTrigger = 0.0f;
        mGamepadRightTrigger = 0.0f;
        resetGamepadStateNative();
    }

    @Override
    public boolean dispatchKeyEvent(final KeyEvent event) {
        final int keyCode = event.getKeyCode();
        if (!isGamepadEvent(event) && !isDedicatedGamepadKey(keyCode)) {
            return super.dispatchKeyEvent(event);
        }

        final boolean isDown = event.getAction() == KeyEvent.ACTION_DOWN;
        final boolean isUp = event.getAction() == KeyEvent.ACTION_UP;
        if (isDown || isUp) {
            final int button = xInputButtonForKeyCode(keyCode);
            if (button != 0) {
                if (isDown) {
                    mGamepadKeyButtons |= button;
                } else {
                    mGamepadKeyButtons &= ~button;
                }
            } else if (keyCode == KeyEvent.KEYCODE_BUTTON_L2) {
                mGamepadLeftTriggerDigital = isDown;
            } else if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) {
                mGamepadRightTriggerDigital = isDown;
            }
            pushGamepadState();
        }

        // Gamepad B is reported as KEYCODE_BACK by some devices. Never let a
        // gamepad-source key event finish the emulation activity.
        return true;
    }

    @Override
    public boolean onGenericMotionEvent(final MotionEvent event) {
        if (!isGamepadEvent(event) || event.getAction() != MotionEvent.ACTION_MOVE) {
            return super.onGenericMotionEvent(event);
        }

        mGamepadLeftX = getStickAxis(event, MotionEvent.AXIS_X);
        mGamepadLeftY = getStickAxis(event, MotionEvent.AXIS_Y);

        final float rightZ = getStickAxis(event, MotionEvent.AXIS_Z);
        final float rightRz = getStickAxis(event, MotionEvent.AXIS_RZ);
        final float rightRx = getStickAxis(event, MotionEvent.AXIS_RX);
        final float rightRy = getStickAxis(event, MotionEvent.AXIS_RY);
        final boolean hasZRz = hasAxis(event, MotionEvent.AXIS_Z)
                || hasAxis(event, MotionEvent.AXIS_RZ);
        final boolean hasRxRy = hasAxis(event, MotionEvent.AXIS_RX)
                || hasAxis(event, MotionEvent.AXIS_RY);
        final float zRzMagnitude = Math.max(Math.abs(rightZ), Math.abs(rightRz));
        final float rxRyMagnitude = Math.max(Math.abs(rightRx), Math.abs(rightRy));
        if (hasRxRy && (!hasZRz || rxRyMagnitude > zRzMagnitude + 0.01f)) {
            mGamepadRightX = rightRx;
            mGamepadRightY = rightRy;
        } else {
            mGamepadRightX = rightZ;
            mGamepadRightY = rightRz;
        }

        mGamepadLeftTrigger = Math.max(
                getTriggerAxis(event, MotionEvent.AXIS_LTRIGGER),
                getTriggerAxis(event, MotionEvent.AXIS_BRAKE));
        mGamepadRightTrigger = Math.max(
                getTriggerAxis(event, MotionEvent.AXIS_RTRIGGER),
                getTriggerAxis(event, MotionEvent.AXIS_GAS));

        final float hatX = getStickAxis(event, MotionEvent.AXIS_HAT_X);
        final float hatY = getStickAxis(event, MotionEvent.AXIS_HAT_Y);
        mGamepadHatButtons = 0;
        if (hatX <= -0.5f) {
            mGamepadHatButtons |= XINPUT_DPAD_LEFT;
        } else if (hatX >= 0.5f) {
            mGamepadHatButtons |= XINPUT_DPAD_RIGHT;
        }
        if (hatY <= -0.5f) {
            mGamepadHatButtons |= XINPUT_DPAD_UP;
        } else if (hatY >= 0.5f) {
            mGamepadHatButtons |= XINPUT_DPAD_DOWN;
        }

        pushGamepadState();
        return true;
    }

    @Override
    protected void onPause() {
        clearGamepadState();
        super.onPause();
    }
}
