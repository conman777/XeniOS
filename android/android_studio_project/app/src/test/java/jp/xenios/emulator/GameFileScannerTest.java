package jp.xenios.emulator;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.Collections;
import java.util.Locale;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public class GameFileScannerTest {
    private Locale originalLocale;
    private File tempRoot;

    @Before
    public void setUp() throws IOException {
        originalLocale = Locale.getDefault();
        tempRoot = File.createTempFile("xenios-games", "");
        assertTrue(tempRoot.delete());
        assertTrue(tempRoot.mkdirs());
    }

    @After
    public void tearDown() {
        Locale.setDefault(originalLocale);
        deleteRecursively(tempRoot);
    }

    @Test
    public void isGameFileUsesLocaleIndependentExtensionMatching() {
        Locale.setDefault(new Locale("tr", "TR"));

        assertTrue(GameFileScanner.isGameFile(new File("HALO_REACH.ISO")));
        assertTrue(GameFileScanner.isGameFile(new File("default.XEX")));
        assertTrue(GameFileScanner.isGameFile(new File("archive.ZAR")));
    }

    @Test
    public void findNewestGameReturnsNewestCandidate() throws IOException {
        final File older = touch(new File(tempRoot, "older.iso"), 1000L);
        final File nestedDir = new File(tempRoot, "nested");
        assertTrue(nestedDir.mkdirs());
        final File newer = touch(new File(nestedDir, "newer.xex"), 2000L);

        assertEquals(newer, GameFileScanner.findNewestGame(Collections.singletonList(tempRoot)));
        assertTrue(older.exists());
    }

    @Test
    public void findNewestGameHandlesMissingRoots() {
        assertNull(GameFileScanner.findNewestGame(Arrays.asList(null, new File(tempRoot, "missing"))));
    }

    private static File touch(final File file, final long modifiedTime) throws IOException {
        assertTrue(file.createNewFile());
        assertTrue(file.setLastModified(modifiedTime));
        return file;
    }

    private static void deleteRecursively(final File file) {
        if (file == null || !file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            final File[] children = file.listFiles();
            if (children != null) {
                for (final File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        file.delete();
    }
}
