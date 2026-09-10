package jp.xenios.emulator;

import static org.junit.Assert.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import org.junit.Test;

public class DebugDiagnosticsBuildGuardTest {
    @Test
    public void panelDefaultsHiddenAndRequiresBuildConfigDebug() throws IOException {
        final String layout = read("src/main/res/layout/activity_emulator.xml");
        final String activity =
                read("src/main/java/jp/xenios/emulator/EmulatorActivity.java");
        final String windowedActivity =
                read("src/main/java/jp/xenios/emulator/WindowedAppActivity.java");

        assertTrue(layout.contains(
                "android:id=\"@+id/debug_diagnostics_panel\""));
        assertTrue(layout.contains(
                "android:id=\"@+id/report_diagnostics_button\""));
        assertTrue(layout.contains(
                "android:id=\"@+id/capture_diagnostics_button\""));
        assertTrue(layout.contains(
                "android:id=\"@+id/restore_diagnostic_save_button\""));
        assertTrue(layout.contains("android:focusable=\"false\""));
        assertTrue(layout.contains("android:visibility=\"gone\""));
        assertTrue(activity.contains("if (!BuildConfig.DEBUG)"));
        assertTrue(activity.contains("panel.setVisibility(View.GONE)"));
        assertTrue(activity.contains("toggle.setOnClickListener(view ->"));
        assertTrue(activity.contains(
                "panel.setVisibility(show ? View.VISIBLE : View.GONE)"));
        assertTrue(activity.contains("DiagnosticCaptureManager.captureReport("));
        assertTrue(activity.contains("setWindowPaintingSuspended(true)"));
        assertTrue(activity.contains("isDedicatedGamepadKey(keyCode)"));
        assertTrue(activity.contains("beginLongDiagnosticOperation("));
        assertTrue(windowedActivity.contains(
                "mAppContext == 0 || mWindowPaintingSuspended"));
    }

    private static String read(final String path) throws IOException {
        return new String(
                Files.readAllBytes(Paths.get(path)),
                StandardCharsets.UTF_8);
    }
}
