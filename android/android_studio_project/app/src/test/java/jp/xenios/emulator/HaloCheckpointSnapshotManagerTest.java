package jp.xenios.emulator;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public class HaloCheckpointSnapshotManagerTest {
    private File filesDirectory;
    private File autosaveDirectory;

    @Before
    public void setUp() throws IOException {
        HaloCheckpointSnapshotManager.resetSessionGuardForTests();
        filesDirectory = Files.createTempDirectory("xenios-halo-checkpoint").toFile();
        autosaveDirectory = new File(filesDirectory, "cache1/autosave");
        assertTrue(autosaveDirectory.mkdirs());
        write(new File(autosaveDirectory, "checkpoint.bin"), "known-problem-point");
        final File nested = new File(autosaveDirectory, "metadata");
        assertTrue(nested.mkdirs());
        write(new File(nested, "state.bin"), "stable");
    }

    @After
    public void tearDown() {
        HaloCheckpointSnapshotManager.resetSessionGuardForTests();
        deleteRecursively(filesDirectory);
    }

    @Test
    public void snapshotsAreImmutableAndNeverOverwriteEachOther() throws Exception {
        final HaloCheckpointSnapshotManager.SnapshotInfo first =
                HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
        write(new File(autosaveDirectory, "checkpoint.bin"), "later-checkpoint");
        final HaloCheckpointSnapshotManager.SnapshotInfo second =
                HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");

        assertNotEquals(first.id, second.id);
        assertNotEquals(first.sha256, second.sha256);
        assertEquals(
                "known-problem-point",
                read(new File(first.directory, "autosave/checkpoint.bin")));
        assertEquals(
                "later-checkpoint",
                read(new File(second.directory, "autosave/checkpoint.bin")));
        assertEquals(2, HaloCheckpointSnapshotManager.listSnapshots(filesDirectory).size());
    }

    @Test
    public void restorePreservesCurrentAutosaveAndSnapshot() throws Exception {
        final HaloCheckpointSnapshotManager.SnapshotInfo problemPoint =
                HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
        write(new File(autosaveDirectory, "checkpoint.bin"), "current-progress");

        final HaloCheckpointSnapshotManager.RestoreResult restored =
                HaloCheckpointSnapshotManager.restoreSnapshot(filesDirectory, problemPoint.id);

        assertEquals("known-problem-point", read(new File(autosaveDirectory, "checkpoint.bin")));
        assertTrue(problemPoint.directory.isDirectory());
        assertEquals(
                "known-problem-point",
                read(new File(problemPoint.directory, "autosave/checkpoint.bin")));
        assertTrue(restored.recoverySnapshot.directory.isDirectory());
        assertEquals(
                "current-progress",
                read(new File(restored.recoverySnapshot.directory, "autosave/checkpoint.bin")));
        assertTrue(restored.retainedOriginal.isDirectory());
        assertEquals(
                "current-progress",
                read(new File(restored.retainedOriginal, "checkpoint.bin")));
    }

    @Test
    public void corruptSnapshotIsRejectedBeforeLiveAutosaveChanges() throws Exception {
        final HaloCheckpointSnapshotManager.SnapshotInfo snapshot =
                HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
        write(new File(snapshot.directory, "autosave/checkpoint.bin"), "tampered");
        write(new File(autosaveDirectory, "checkpoint.bin"), "current-progress");

        try {
            HaloCheckpointSnapshotManager.restoreSnapshot(filesDirectory, snapshot.id);
            fail("Expected corrupt snapshot to be rejected.");
        } catch (final IOException expected) {
            assertTrue(expected.getMessage().contains("checksum"));
        }

        assertEquals("current-progress", read(new File(autosaveDirectory, "checkpoint.bin")));
        assertTrue(snapshot.directory.isDirectory());
        assertEquals(0, HaloCheckpointSnapshotManager.listSnapshots(filesDirectory).size());
    }

    @Test
    public void missingAutosaveCannotBeSnapshottedOrRestored() throws Exception {
        final HaloCheckpointSnapshotManager.SnapshotInfo snapshot =
                HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
        deleteRecursively(autosaveDirectory);
        assertFalse(HaloCheckpointSnapshotManager.hasAutosave(filesDirectory));

        try {
            HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
            fail("Expected snapshot creation to fail.");
        } catch (final IOException expected) {
            assertTrue(expected.getMessage().contains("autosave"));
        }

        try {
            HaloCheckpointSnapshotManager.restoreSnapshot(filesDirectory, snapshot.id);
            fail("Expected restore to fail.");
        } catch (final IOException expected) {
            assertTrue(expected.getMessage().contains("autosave"));
        }
    }

    @Test
    public void checkpointAccessLocksAfterHaloStartsInProcess() throws Exception {
        final HaloCheckpointSnapshotManager.SnapshotInfo snapshot =
                HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
        HaloCheckpointSnapshotManager.markEmulatorStartedInThisProcess();
        assertFalse(HaloCheckpointSnapshotManager.isCheckpointAccessAllowed());

        try {
            HaloCheckpointSnapshotManager.createSnapshot(filesDirectory, "manual");
            fail("Expected snapshot creation to be locked.");
        } catch (final IOException expected) {
            assertTrue(expected.getMessage().contains("Fully stop and restart"));
        }

        try {
            HaloCheckpointSnapshotManager.restoreSnapshot(filesDirectory, snapshot.id);
            fail("Expected restore to be locked.");
        } catch (final IOException expected) {
            assertTrue(expected.getMessage().contains("Fully stop and restart"));
        }
        assertEquals("known-problem-point", read(new File(autosaveDirectory, "checkpoint.bin")));
    }

    private static void write(final File file, final String contents) throws IOException {
        final File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Could not create " + parent);
        }
        try (FileOutputStream output = new FileOutputStream(file, false)) {
            output.write(contents.getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
    }

    private static String read(final File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
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
