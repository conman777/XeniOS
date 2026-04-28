package jp.xenios.emulator;

import static org.junit.Assert.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.regex.Pattern;
import org.junit.Test;

public class AndroidManifestSecurityTest {
    private static final String MANIFEST_PATH = "src/main/AndroidManifest.xml";

    @Test
    public void backupIsDisabled() throws IOException {
        final String manifest = readManifest();

        assertTrue(manifest.contains("android:allowBackup=\"false\""));
    }

    @Test
    public void internalActivitiesAreNotExported() throws IOException {
        final String manifest = readManifest();

        assertActivityExported(manifest, "jp.xenios.emulator.GpuTraceViewerActivity", false);
        assertActivityExported(manifest, "jp.xenios.emulator.EmulatorActivity", false);
        assertActivityExported(manifest, "jp.xenios.emulator.WindowDemoActivity", false);
        assertActivityExported(manifest, "jp.xenios.emulator.LauncherActivity", true);
    }

    private static String readManifest() throws IOException {
        return new String(Files.readAllBytes(Paths.get(MANIFEST_PATH)), StandardCharsets.UTF_8);
    }

    private static void assertActivityExported(
            final String manifest, final String activityName, final boolean exported) {
        final String expected = Boolean.toString(exported);
        final Pattern pattern = Pattern.compile(
                "<activity[\\s\\S]*?android:name=\"" + Pattern.quote(activityName)
                        + "\"[\\s\\S]*?android:exported=\"" + expected + "\"[\\s\\S]*?>");
        assertTrue(activityName + " exported must be " + expected, pattern.matcher(manifest).find());
    }
}
