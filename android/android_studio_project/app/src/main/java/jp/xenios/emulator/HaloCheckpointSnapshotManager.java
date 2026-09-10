package jp.xenios.emulator;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Properties;
import java.util.TimeZone;
import java.util.UUID;

final class HaloCheckpointSnapshotManager {
    private static final String SNAPSHOT_ROOT = "debug_checkpoint_snapshots/halo_reach";
    private static final String AUTOSAVE_PATH = "cache1/autosave";
    private static final String PAYLOAD_NAME = "autosave";
    private static final String METADATA_NAME = "metadata.properties";
    private static final int COPY_BUFFER_SIZE = 64 * 1024;
    private static boolean emulatorStartedInThisProcess;

    static final class SnapshotInfo {
        final String id;
        final long createdUtcMillis;
        final String reason;
        final String sha256;
        final File directory;

        SnapshotInfo(
                final String id,
                final long createdUtcMillis,
                final String reason,
                final String sha256,
                final File directory) {
            this.id = id;
            this.createdUtcMillis = createdUtcMillis;
            this.reason = reason;
            this.sha256 = sha256;
            this.directory = directory;
        }
    }

    static final class RestoreResult {
        final SnapshotInfo restoredSnapshot;
        final SnapshotInfo recoverySnapshot;
        final File retainedOriginal;

        RestoreResult(
                final SnapshotInfo restoredSnapshot,
                final SnapshotInfo recoverySnapshot,
                final File retainedOriginal) {
            this.restoredSnapshot = restoredSnapshot;
            this.recoverySnapshot = recoverySnapshot;
            this.retainedOriginal = retainedOriginal;
        }
    }

    private HaloCheckpointSnapshotManager() {}

    static synchronized void markEmulatorStartedInThisProcess() {
        emulatorStartedInThisProcess = true;
    }

    static synchronized boolean isCheckpointAccessAllowed() {
        return !emulatorStartedInThisProcess;
    }

    static synchronized void resetSessionGuardForTests() {
        emulatorStartedInThisProcess = false;
    }

    static synchronized SnapshotInfo createSnapshot(
            final File filesDirectory, final String reason) throws IOException {
        requireStoppedSession();
        final File autosave = autosaveDirectory(filesDirectory);
        requireDirectory(autosave, "Halo cache1 autosave");

        final File snapshotRoot = snapshotRoot(filesDirectory);
        ensureDirectory(snapshotRoot);

        final long createdUtcMillis = System.currentTimeMillis();
        final String id = createSnapshotId(createdUtcMillis);
        final File temporaryDirectory =
                new File(snapshotRoot, ".creating-" + UUID.randomUUID().toString());
        final File finalDirectory = new File(snapshotRoot, id);
        if (finalDirectory.exists()) {
            throw new IOException("Snapshot already exists: " + id);
        }
        ensureDirectory(temporaryDirectory);

        try {
            final String sourceDigest = treeSha256(autosave);
            final File payload = new File(temporaryDirectory, PAYLOAD_NAME);
            copyTree(autosave, payload);
            final String copiedDigest = treeSha256(payload);
            if (!sourceDigest.equals(copiedDigest)) {
                throw new IOException("Snapshot checksum mismatch after copy.");
            }

            writeMetadata(
                    new File(temporaryDirectory, METADATA_NAME),
                    id,
                    createdUtcMillis,
                    reason,
                    copiedDigest);
            final SnapshotInfo verified = readAndVerifySnapshot(temporaryDirectory, false);
            if (!temporaryDirectory.renameTo(finalDirectory)) {
                throw new IOException("Could not publish checkpoint snapshot atomically.");
            }
            return new SnapshotInfo(
                    verified.id,
                    verified.createdUtcMillis,
                    verified.reason,
                    verified.sha256,
                    finalDirectory);
        } catch (final IOException e) {
            deleteRecursively(temporaryDirectory);
            throw e;
        }
    }

    static synchronized RestoreResult restoreSnapshot(
            final File filesDirectory, final String snapshotId) throws IOException {
        requireStoppedSession();
        if (!isValidSnapshotId(snapshotId)) {
            throw new IOException("Invalid checkpoint snapshot id.");
        }

        final File snapshotDirectory = new File(snapshotRoot(filesDirectory), snapshotId);
        final SnapshotInfo snapshot = readAndVerifySnapshot(snapshotDirectory, true);
        final File autosave = autosaveDirectory(filesDirectory);
        requireDirectory(autosave, "Current Halo cache1 autosave");

        final SnapshotInfo recovery = createSnapshot(filesDirectory, "pre-restore recovery");
        final File cache1 = autosave.getParentFile();
        final String operationId = UUID.randomUUID().toString();
        final File staged = new File(cache1, ".autosave.restore-" + operationId);
        final File retainedOriginal = new File(cache1, ".autosave.before-restore-" + operationId);
        copyTree(new File(snapshotDirectory, PAYLOAD_NAME), staged);
        if (!snapshot.sha256.equals(treeSha256(staged))) {
            deleteRecursively(staged);
            throw new IOException("Staged restore checksum does not match the snapshot.");
        }

        boolean originalMoved = false;
        try {
            if (!autosave.renameTo(retainedOriginal)) {
                throw new IOException("Could not preserve the current autosave before restore.");
            }
            originalMoved = true;
            if (!staged.renameTo(autosave)) {
                throw new IOException("Could not publish the restored autosave atomically.");
            }
            if (!snapshot.sha256.equals(treeSha256(autosave))) {
                final File failedRestore =
                        new File(cache1, ".autosave.failed-restore-" + operationId);
                if (!autosave.renameTo(failedRestore)
                        || !retainedOriginal.renameTo(autosave)) {
                    throw new IOException(
                            "Restored checksum failed and automatic rollback was incomplete.");
                }
                throw new IOException("Restored checksum failed; the original autosave was restored.");
            }
            return new RestoreResult(snapshot, recovery, retainedOriginal);
        } catch (final IOException e) {
            if (originalMoved && !autosave.exists() && retainedOriginal.exists()) {
                retainedOriginal.renameTo(autosave);
            }
            deleteRecursively(staged);
            throw e;
        }
    }

    static synchronized List<SnapshotInfo> listSnapshots(final File filesDirectory) {
        final File root = snapshotRoot(filesDirectory);
        final File[] children = root.listFiles();
        if (children == null) {
            return Collections.emptyList();
        }

        final List<SnapshotInfo> snapshots = new ArrayList<>();
        for (final File child : children) {
            if (!child.isDirectory() || child.getName().startsWith(".")) {
                continue;
            }
            try {
                snapshots.add(readAndVerifySnapshot(child, true));
            } catch (final IOException ignored) {
                // Invalid or incomplete snapshots are never offered for restore.
            }
        }
        Collections.sort(snapshots, new Comparator<SnapshotInfo>() {
            @Override
            public int compare(final SnapshotInfo left, final SnapshotInfo right) {
                return Long.compare(right.createdUtcMillis, left.createdUtcMillis);
            }
        });
        return snapshots;
    }

    static boolean hasAutosave(final File filesDirectory) {
        final File autosave = autosaveDirectory(filesDirectory);
        return autosave.isDirectory();
    }

    private static SnapshotInfo readAndVerifySnapshot(
            final File directory, final boolean requireMatchingDirectoryName)
            throws IOException {
        requireDirectory(directory, "Checkpoint snapshot");
        final Properties metadata = new Properties();
        final File metadataFile = new File(directory, METADATA_NAME);
        try (FileInputStream input = new FileInputStream(metadataFile)) {
            metadata.load(input);
        }

        final String id = metadata.getProperty("id", "");
        final String reason = metadata.getProperty("reason", "");
        final String expectedDigest = metadata.getProperty("sha256", "");
        final long createdUtcMillis;
        try {
            createdUtcMillis = Long.parseLong(metadata.getProperty("created_utc_millis", ""));
        } catch (final NumberFormatException e) {
            throw new IOException("Invalid checkpoint snapshot timestamp.", e);
        }
        if (!isValidSnapshotId(id)
                || (requireMatchingDirectoryName && !directory.getName().equals(id))) {
            throw new IOException("Checkpoint snapshot id does not match its directory.");
        }
        if (!expectedDigest.matches("[0-9a-f]{64}")) {
            throw new IOException("Invalid checkpoint snapshot checksum.");
        }

        final String actualDigest = treeSha256(new File(directory, PAYLOAD_NAME));
        if (!expectedDigest.equals(actualDigest)) {
            throw new IOException("Checkpoint snapshot checksum validation failed.");
        }
        return new SnapshotInfo(id, createdUtcMillis, reason, expectedDigest, directory);
    }

    private static void writeMetadata(
            final File file,
            final String id,
            final long createdUtcMillis,
            final String reason,
            final String sha256) throws IOException {
        final Properties metadata = new Properties();
        metadata.setProperty("version", "1");
        metadata.setProperty("id", id);
        metadata.setProperty("created_utc_millis", Long.toString(createdUtcMillis));
        metadata.setProperty("reason", reason);
        metadata.setProperty("source", AUTOSAVE_PATH);
        metadata.setProperty("sha256", sha256);
        try (FileOutputStream output = new FileOutputStream(file)) {
            metadata.store(output, "XeniOS Halo Reach normal autosave checkpoint snapshot");
            output.getFD().sync();
        }
    }

    private static void copyTree(final File source, final File destination) throws IOException {
        requireInsideTree(source, source);
        if (source.isDirectory()) {
            ensureDirectory(destination);
            final File[] children = source.listFiles();
            if (children == null) {
                throw new IOException("Could not list directory: " + source);
            }
            Arrays.sort(children, new Comparator<File>() {
                @Override
                public int compare(final File left, final File right) {
                    return left.getName().compareTo(right.getName());
                }
            });
            for (final File child : children) {
                requireInsideTree(source, child);
                copyTree(child, new File(destination, child.getName()));
            }
            return;
        }
        if (!source.isFile()) {
            throw new IOException("Unsupported autosave entry: " + source);
        }
        final File parent = destination.getParentFile();
        ensureDirectory(parent);
        try (BufferedInputStream input =
                     new BufferedInputStream(new FileInputStream(source), COPY_BUFFER_SIZE);
             FileOutputStream fileOutput = new FileOutputStream(destination);
             BufferedOutputStream output =
                     new BufferedOutputStream(fileOutput, COPY_BUFFER_SIZE)) {
            final byte[] buffer = new byte[COPY_BUFFER_SIZE];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read != 0) {
                    output.write(buffer, 0, read);
                }
            }
            output.flush();
            fileOutput.getFD().sync();
        }
    }

    private static String treeSha256(final File root) throws IOException {
        requireDirectory(root, "Autosave tree");
        final MessageDigest digest = newSha256();
        updateTreeDigest(root, root, digest);
        return toHex(digest.digest());
    }

    private static void updateTreeDigest(
            final File root, final File entry, final MessageDigest digest) throws IOException {
        requireInsideTree(root, entry);
        final String relativePath =
                root.equals(entry)
                        ? "."
                        : root.toURI().relativize(entry.toURI()).getPath();
        digest.update((byte) (entry.isDirectory() ? 'D' : 'F'));
        digest.update(relativePath.getBytes(StandardCharsets.UTF_8));
        digest.update((byte) 0);

        if (entry.isDirectory()) {
            final File[] children = entry.listFiles();
            if (children == null) {
                throw new IOException("Could not list directory: " + entry);
            }
            Arrays.sort(children, new Comparator<File>() {
                @Override
                public int compare(final File left, final File right) {
                    return left.getName().compareTo(right.getName());
                }
            });
            for (final File child : children) {
                updateTreeDigest(root, child, digest);
            }
            return;
        }
        if (!entry.isFile()) {
            throw new IOException("Unsupported autosave entry: " + entry);
        }
        final long length = entry.length();
        for (int shift = 56; shift >= 0; shift -= 8) {
            digest.update((byte) (length >>> shift));
        }
        try (BufferedInputStream input =
                     new BufferedInputStream(new FileInputStream(entry), COPY_BUFFER_SIZE)) {
            final byte[] buffer = new byte[COPY_BUFFER_SIZE];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read != 0) {
                    digest.update(buffer, 0, read);
                }
            }
        }
    }

    private static void requireInsideTree(final File root, final File entry) throws IOException {
        final String rootPath = root.getCanonicalPath();
        final String entryPath = entry.getCanonicalPath();
        if (!entryPath.equals(rootPath)
                && !entryPath.startsWith(rootPath + File.separator)) {
            throw new IOException("Autosave entry escapes its root: " + entry);
        }
    }

    private static MessageDigest newSha256() throws IOException {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (final NoSuchAlgorithmException e) {
            throw new IOException("SHA-256 is unavailable.", e);
        }
    }

    private static String toHex(final byte[] bytes) {
        final StringBuilder result = new StringBuilder(bytes.length * 2);
        for (final byte value : bytes) {
            result.append(String.format(Locale.ROOT, "%02x", value & 0xFF));
        }
        return result.toString();
    }

    private static String createSnapshotId(final long createdUtcMillis) {
        final SimpleDateFormat format =
                new SimpleDateFormat("'snapshot-'yyyyMMdd-HHmmss-SSS", Locale.ROOT);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date(createdUtcMillis))
                + "-"
                + UUID.randomUUID().toString().substring(0, 8);
    }

    private static boolean isValidSnapshotId(final String id) {
        return id != null && id.matches("snapshot-[0-9]{8}-[0-9]{6}-[0-9]{3}-[0-9a-f]{8}");
    }

    private static File autosaveDirectory(final File filesDirectory) {
        return new File(filesDirectory, AUTOSAVE_PATH);
    }

    private static File snapshotRoot(final File filesDirectory) {
        return new File(filesDirectory, SNAPSHOT_ROOT);
    }

    private static void ensureDirectory(final File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Could not create directory: " + directory);
        }
    }

    private static void requireDirectory(final File directory, final String label)
            throws IOException {
        if (!directory.isDirectory()) {
            throw new IOException(label + " directory not found: " + directory);
        }
    }

    private static void requireStoppedSession() throws IOException {
        if (emulatorStartedInThisProcess) {
            throw new IOException(
                    "Checkpoint access is locked because Halo ran in this process. "
                            + "Fully stop and restart XeniOS first.");
        }
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
