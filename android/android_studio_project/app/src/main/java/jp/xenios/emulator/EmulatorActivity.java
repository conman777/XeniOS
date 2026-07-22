package jp.xenios.emulator;

import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

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
        super.onCreate(savedInstanceState);
        if (!isWindowedAppReady()) {
            return;
        }

        setContentView(R.layout.activity_emulator);
        setWindowSurfaceView(findViewById(R.id.emulator_surface_view));
    }

    private static boolean isGamepadEvent(final KeyEvent event) {
        return event.isFromSource(InputDevice.SOURCE_GAMEPAD)
                || event.isFromSource(InputDevice.SOURCE_JOYSTICK);
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
        if (!isGamepadEvent(event)) {
            return super.dispatchKeyEvent(event);
        }

        final boolean isDown = event.getAction() == KeyEvent.ACTION_DOWN;
        final boolean isUp = event.getAction() == KeyEvent.ACTION_UP;
        if (isDown || isUp) {
            final int keyCode = event.getKeyCode();
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
