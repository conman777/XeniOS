package jp.xenios.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

public class LauncherActivity extends Activity {
    private static final int REQUEST_OPEN_GPU_TRACE_VIEWER = 0;
    private static final int REQUEST_PICK_GAME = 1;
    private static final String PREFS_NAME = "xenios_launcher";
    private static final String PREF_LAST_GAME_PATH = "last_game_path";
    private static final String PREF_PROFILE_BOOTSTRAPPED = "profile_bootstrapped";
    private static final String DEFAULT_PROFILE_ASSET = "xenios_android_default_profile.txt";
    private static final String DEFAULT_PROFILE_FILENAME = "xenios_android_profile.txt";

    private Button launchGameButton;
    private Button createCheckpointSnapshotButton;
    private Button restoreCheckpointSnapshotButton;
    private TextView gamePathView;
    private TextView statusView;
    private TextView checkpointStatusView;
    private String detectedGamePath;
    private boolean checkpointOperationRunning;

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_launcher);
        launchGameButton = findViewById(R.id.launch_game_button);
        gamePathView = findViewById(R.id.launcher_game_path);
        statusView = findViewById(R.id.launcher_status);
        configureCheckpointPanel();
        ensureDefaultProfileInstalled();
        refreshDetectedGame();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshDetectedGame();
        refreshCheckpointPanel();
    }

    @Override
    protected void onActivityResult(
            final int requestCode, final int resultCode, final Intent data) {
        if (requestCode == REQUEST_OPEN_GPU_TRACE_VIEWER && resultCode == RESULT_OK) {
            final Uri uri = data.getData();
            if (uri != null) {
                final Intent gpuTraceViewerIntent = new Intent(this, GpuTraceViewerActivity.class);
                final Bundle gpuTraceViewerLaunchArguments = new Bundle();
                gpuTraceViewerLaunchArguments.putString("target_trace_file", uri.toString());
                gpuTraceViewerIntent.putExtra(
                        WindowedAppActivity.EXTRA_CVARS, gpuTraceViewerLaunchArguments);
                startActivity(gpuTraceViewerIntent);
            }
            return;
        }

        if (requestCode == REQUEST_PICK_GAME && resultCode == RESULT_OK && data != null) {
            final Uri uri = data.getData();
            if (uri != null) {
                importPickedGame(uri);
            }
        }
    }

    public void onLaunchGpuTraceViewerClick(final View view) {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        startActivityForResult(intent, REQUEST_OPEN_GPU_TRACE_VIEWER);
    }

    public void onPickGameClick(final View view) {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(
                Intent.EXTRA_MIME_TYPES,
                new String[] {"application/x-iso9660-image", "application/octet-stream"});
        startActivityForResult(intent, REQUEST_PICK_GAME);
    }

    public void onLaunchWindowDemoClick(final View view) {
        startActivity(new Intent(this, WindowDemoActivity.class));
    }

    public void onLaunchDetectedGameClick(final View view) {
        if (detectedGamePath == null || checkpointOperationRunning) {
            return;
        }
        HaloCheckpointSnapshotManager.markEmulatorStartedInThisProcess();
        refreshCheckpointPanel();
        rememberLastGamePath(detectedGamePath);
        final Intent emulatorIntent = new Intent(this, EmulatorActivity.class);
        emulatorIntent.putExtra(
                WindowedAppActivity.EXTRA_CVARS, buildEmulatorLaunchArguments(detectedGamePath));
        startActivity(emulatorIntent);
    }

    public void onRescanGamesClick(final View view) {
        refreshDetectedGame();
    }

    public void onCreateCheckpointSnapshotClick(final View view) {
        if (!BuildConfig.DEBUG || !checkpointActionsAllowed()) {
            showCheckpointError(getString(R.string.checkpoint_restart_required));
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.checkpoint_create_title)
                .setMessage(R.string.checkpoint_create_confirmation)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(
                        R.string.checkpoint_create_button,
                        (dialog, which) -> createCheckpointSnapshot())
                .show();
    }

    public void onRestoreCheckpointSnapshotClick(final View view) {
        if (!BuildConfig.DEBUG || !checkpointActionsAllowed()) {
            showCheckpointError(getString(R.string.checkpoint_restart_required));
            return;
        }
        final List<HaloCheckpointSnapshotManager.SnapshotInfo> snapshots =
                HaloCheckpointSnapshotManager.listSnapshots(getFilesDir());
        if (snapshots.isEmpty()) {
            showCheckpointError(getString(R.string.checkpoint_no_snapshots));
            return;
        }

        final String[] labels = new String[snapshots.size()];
        for (int index = 0; index < snapshots.size(); ++index) {
            final HaloCheckpointSnapshotManager.SnapshotInfo snapshot = snapshots.get(index);
            labels[index] = getString(
                    R.string.checkpoint_snapshot_label,
                    snapshot.id,
                    snapshot.sha256.substring(0, 12));
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.checkpoint_restore_choose_title)
                .setItems(
                        labels,
                        (dialog, which) -> confirmRestoreCheckpointSnapshot(snapshots.get(which)))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void configureCheckpointPanel() {
        final View panel = findViewById(R.id.debug_checkpoint_panel);
        if (!BuildConfig.DEBUG) {
            panel.setVisibility(View.GONE);
            return;
        }
        panel.setVisibility(View.VISIBLE);
        createCheckpointSnapshotButton = findViewById(R.id.create_checkpoint_snapshot_button);
        restoreCheckpointSnapshotButton = findViewById(R.id.restore_checkpoint_snapshot_button);
        checkpointStatusView = findViewById(R.id.checkpoint_snapshot_status);
        refreshCheckpointPanel();
    }

    private void refreshCheckpointPanel() {
        if (!BuildConfig.DEBUG || checkpointStatusView == null) {
            return;
        }
        final boolean allowed = checkpointActionsAllowed();
        final boolean hasAutosave =
                HaloCheckpointSnapshotManager.hasAutosave(getFilesDir());
        final List<HaloCheckpointSnapshotManager.SnapshotInfo> snapshots =
                HaloCheckpointSnapshotManager.listSnapshots(getFilesDir());
        createCheckpointSnapshotButton.setEnabled(allowed && hasAutosave);
        restoreCheckpointSnapshotButton.setEnabled(
                allowed && hasAutosave && !snapshots.isEmpty());
        if (!allowed) {
            checkpointStatusView.setText(R.string.checkpoint_restart_required);
        } else if (!hasAutosave) {
            checkpointStatusView.setText(R.string.checkpoint_autosave_missing);
        } else {
            checkpointStatusView.setText(
                    getString(R.string.checkpoint_ready_status, snapshots.size()));
        }
    }

    private boolean checkpointActionsAllowed() {
        return !checkpointOperationRunning
                && HaloCheckpointSnapshotManager.isCheckpointAccessAllowed();
    }

    private void createCheckpointSnapshot() {
        beginCheckpointOperation();
        new Thread(() -> {
            try {
                final HaloCheckpointSnapshotManager.SnapshotInfo snapshot =
                        HaloCheckpointSnapshotManager.createSnapshot(
                                getFilesDir(), "user-confirmed stable checkpoint");
                runOnUiThread(() -> finishCheckpointOperation(
                        getString(
                                R.string.checkpoint_create_success,
                                snapshot.id,
                                snapshot.sha256.substring(0, 12))));
            } catch (final Exception e) {
                runOnUiThread(() -> finishCheckpointOperation(
                        getString(R.string.checkpoint_operation_failed, e.getMessage())));
            }
        }, "Halo checkpoint snapshot").start();
    }

    private void confirmRestoreCheckpointSnapshot(
            final HaloCheckpointSnapshotManager.SnapshotInfo snapshot) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.checkpoint_restore_title)
                .setMessage(getString(R.string.checkpoint_restore_confirmation, snapshot.id))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(
                        R.string.checkpoint_restore_button,
                        (dialog, which) -> restoreCheckpointSnapshot(snapshot))
                .show();
    }

    private void restoreCheckpointSnapshot(
            final HaloCheckpointSnapshotManager.SnapshotInfo snapshot) {
        beginCheckpointOperation();
        new Thread(() -> {
            try {
                final HaloCheckpointSnapshotManager.RestoreResult result =
                        HaloCheckpointSnapshotManager.restoreSnapshot(
                                getFilesDir(), snapshot.id);
                runOnUiThread(() -> finishCheckpointOperation(
                        getString(
                                R.string.checkpoint_restore_success,
                                result.restoredSnapshot.id,
                                result.recoverySnapshot.id)));
            } catch (final Exception e) {
                runOnUiThread(() -> finishCheckpointOperation(
                        getString(R.string.checkpoint_operation_failed, e.getMessage())));
            }
        }, "Halo checkpoint restore").start();
    }

    private void beginCheckpointOperation() {
        checkpointOperationRunning = true;
        launchGameButton.setEnabled(false);
        createCheckpointSnapshotButton.setEnabled(false);
        restoreCheckpointSnapshotButton.setEnabled(false);
        checkpointStatusView.setText(R.string.checkpoint_operation_running);
    }

    private void finishCheckpointOperation(final String message) {
        checkpointOperationRunning = false;
        refreshDetectedGame();
        refreshCheckpointPanel();
        checkpointStatusView.setText(message);
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private void showCheckpointError(final String message) {
        checkpointStatusView.setText(message);
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private void importPickedGame(final Uri uri) {
        final File gamesDir = getExternalFilesDir("games");
        if (gamesDir == null) {
            Toast.makeText(this, R.string.launcher_import_failed, Toast.LENGTH_LONG).show();
            return;
        }
        gamesDir.mkdirs();

        String fileName = queryDisplayName(uri);
        if (fileName == null || fileName.isEmpty()) {
            fileName = "imported-game.iso";
        }
        if (!GameFileScanner.isGameFile(new File(fileName))) {
            fileName = fileName + ".iso";
        }

        final File destination = new File(gamesDir, fileName);
        try (InputStream inputStream = getContentResolver().openInputStream(uri);
             OutputStream outputStream = new FileOutputStream(destination)) {
            if (inputStream == null) {
                throw new IllegalStateException("Unable to open selected file.");
            }
            final byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = inputStream.read(buffer)) >= 0) {
                if (read == 0) {
                    continue;
                }
                outputStream.write(buffer, 0, read);
            }
            outputStream.flush();
            try {
                final int takeFlags =
                        dataFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                getContentResolver().takePersistableUriPermission(uri, takeFlags);
            } catch (SecurityException ignored) {
                // Best effort only; copied file is enough to launch.
            }
            Toast.makeText(
                    this,
                    getString(R.string.launcher_import_success, destination.getAbsolutePath()),
                    Toast.LENGTH_LONG).show();
            refreshDetectedGame();
        } catch (final Exception e) {
            Toast.makeText(
                    this,
                    getString(R.string.launcher_import_failed_with_reason, e.getMessage()),
                    Toast.LENGTH_LONG).show();
        }
    }

    private void ensureDefaultProfileInstalled() {
        final SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        if (prefs.getBoolean(PREF_PROFILE_BOOTSTRAPPED, false)) {
            return;
        }

        final File profileDestination = new File(getFilesDir(), DEFAULT_PROFILE_FILENAME);
        if (profileDestination.exists()) {
            prefs.edit().putBoolean(PREF_PROFILE_BOOTSTRAPPED, true).apply();
            return;
        }

        try {
            final AssetManager assets = getAssets();
            try (InputStream inputStream = assets.open(DEFAULT_PROFILE_ASSET);
                 OutputStream outputStream = new FileOutputStream(profileDestination)) {
                final byte[] buffer = new byte[8192];
                int read;
                while ((read = inputStream.read(buffer)) >= 0) {
                    if (read == 0) {
                        continue;
                    }
                    outputStream.write(buffer, 0, read);
                }
                outputStream.flush();
            }
            prefs.edit().putBoolean(PREF_PROFILE_BOOTSTRAPPED, true).apply();
        } catch (final Exception ignored) {
            // Native defaults still apply if asset copy fails.
        }
    }

    private void rememberLastGamePath(final String gamePath) {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .edit()
                .putString(PREF_LAST_GAME_PATH, gamePath)
                .apply();
    }

    private static int dataFlags(final int flag) {
        return flag & Intent.FLAG_GRANT_READ_URI_PERMISSION;
    }

    private String queryDisplayName(final Uri uri) {
        Cursor cursor = null;
        try {
            cursor = getContentResolver().query(uri, null, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                final int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (nameIndex >= 0) {
                    return cursor.getString(nameIndex);
                }
            }
        } finally {
            if (cursor != null) {
                cursor.close();
            }
        }
        final String lastSegment = uri.getLastPathSegment();
        if (lastSegment == null) {
            return null;
        }
        final int slashIndex = lastSegment.lastIndexOf('/');
        if (slashIndex >= 0 && slashIndex + 1 < lastSegment.length()) {
            return lastSegment.substring(slashIndex + 1);
        }
        return lastSegment;
    }

    private Bundle buildEmulatorLaunchArguments(final String targetPath) {
        final File storageRoot = getFilesDir();
        final File contentRoot = new File(storageRoot, "content");
        final Bundle launchArguments = new Bundle();
        contentRoot.mkdirs();

        launchArguments.putString("target", targetPath);
        launchArguments.putString("storage_root", storageRoot.getAbsolutePath());
        launchArguments.putString("content_root", contentRoot.getAbsolutePath());
        launchArguments.putString("cache_root", getCacheDir().getAbsolutePath());
        launchArguments.putString("apu", "opensl");
        launchArguments.putString("gpu", "vulkan");
        launchArguments.putString("hid", "nop");
        launchArguments.putBoolean("discord", false);
        launchArguments.putBoolean("a64_fail_fast_on_access_violation", false);
        launchArguments.putBoolean("log_undefined_extern_args", true);
        launchArguments.putInt("a64_watch_store_address", (int) 0x82BD4128L);
        if (isProbablyEmulator()) {
            launchArguments.putLong("framerate_limit", 30);
            launchArguments.putBoolean("async_shader_compilation", false);
            launchArguments.putInt("vulkan_pipeline_creation_threads", 1);
            launchArguments.putString("xma_decoder", "old");
        }
        return launchArguments;
    }

    private void refreshDetectedGame() {
        ensureGameDirectories();
        final File detectedGame = findDetectedGame();
        if (detectedGame == null) {
            final String rememberedPath =
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                            .getString(PREF_LAST_GAME_PATH, null);
            if (rememberedPath != null) {
                final File rememberedGame = new File(rememberedPath);
                if (rememberedGame.isFile() && GameFileScanner.isGameFile(rememberedGame)) {
                    detectedGamePath = rememberedGame.getAbsolutePath();
                    launchGameButton.setEnabled(true);
                    updateStatusViews(getString(R.string.launcher_status_ready), detectedGamePath);
                    return;
                }
            }
        }
        detectedGamePath = detectedGame != null ? detectedGame.getAbsolutePath() : null;
        launchGameButton.setEnabled(detectedGamePath != null);

        if (detectedGamePath != null) {
            updateStatusViews(getString(R.string.launcher_status_ready), detectedGamePath);
        } else {
            updateStatusViews(
                    getString(R.string.launcher_status_missing_game),
                    getString(R.string.launcher_game_path_hint, getPreferredImportDirectory()));
        }
    }

    private void updateStatusViews(final String statusText, final String gamePathText) {
        final StringBuilder status = new StringBuilder();
        if (isProbablyEmulator()) {
            status.append(getString(R.string.launcher_status_emulator_warning)).append('\n');
        }
        status.append(statusText);
        statusView.setText(status.toString().trim());
        gamePathView.setText(gamePathText);
    }

    private File findDetectedGame() {
        return GameFileScanner.findNewestGame(getCandidateRoots());
    }

    private void ensureGameDirectories() {
        final File internalGamesDir = new File(getFilesDir(), "games");
        internalGamesDir.mkdirs();
        final File externalGamesDir = getExternalFilesDir("games");
        if (externalGamesDir != null) {
            externalGamesDir.mkdirs();
        }
    }

    private List<File> getCandidateRoots() {
        final List<File> roots = new ArrayList<>();
        roots.add(new File(getFilesDir(), "games"));
        roots.add(getFilesDir());
        final File externalFilesDir = getExternalFilesDir(null);
        if (externalFilesDir != null) {
            roots.add(externalFilesDir);
        }
        final File externalGamesDir = getExternalFilesDir("games");
        if (externalGamesDir != null) {
            roots.add(externalGamesDir);
        }
        return roots;
    }

    private String getPreferredImportDirectory() {
        final File externalGamesDir = getExternalFilesDir("games");
        if (externalGamesDir != null) {
            return externalGamesDir.getAbsolutePath();
        }
        return new File(getFilesDir(), "games").getAbsolutePath();
    }

    private static boolean isProbablyEmulator() {
        return Build.FINGERPRINT.startsWith("generic")
                || Build.FINGERPRINT.contains("emulator")
                || Build.FINGERPRINT.contains("/emu")
                || Build.MODEL.contains("Emulator")
                || Build.MODEL.contains("Android SDK built for x86")
                || Build.MODEL.startsWith("sdk_")
                || Build.BRAND.startsWith("generic")
                || Build.DEVICE.startsWith("generic")
                || Build.DEVICE.startsWith("emu")
                || Build.HARDWARE.equals("goldfish")
                || Build.HARDWARE.equals("ranchu")
                || Build.PRODUCT.startsWith("sdk_")
                || "google_sdk".equals(Build.PRODUCT);
    }
}
