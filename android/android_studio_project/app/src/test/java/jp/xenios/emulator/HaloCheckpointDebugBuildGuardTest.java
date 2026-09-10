package jp.xenios.emulator;

import static org.junit.Assert.assertTrue;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import org.junit.Test;

public class HaloCheckpointDebugBuildGuardTest {
    @Test
    public void checkpointPanelIsHiddenOutsideDebugBuilds() throws Exception {
        final String launcher = read(
                "src/main/java/jp/xenios/emulator/LauncherActivity.java");
        final String layout = read("src/main/res/layout/activity_launcher.xml");

        assertTrue(launcher.contains("if (!BuildConfig.DEBUG)"));
        assertTrue(launcher.contains("panel.setVisibility(View.GONE)"));
        assertTrue(layout.contains("android:id=\"@+id/debug_checkpoint_panel\""));
        assertTrue(layout.contains("android:visibility=\"gone\""));
    }

    private static String read(final String path) throws Exception {
        return new String(
                Files.readAllBytes(Paths.get(path)),
                StandardCharsets.UTF_8);
    }
}
