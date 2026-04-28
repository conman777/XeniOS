package jp.xenios.emulator;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class LauncherActivity extends Activity {
    private static final int REQUEST_OPEN_GPU_TRACE_VIEWER = 0;

    private Button launchGameButton;
    private TextView gamePathView;
    private TextView statusView;
    private String detectedGamePath;

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_launcher);
        launchGameButton = findViewById(R.id.launch_game_button);
        gamePathView = findViewById(R.id.launcher_game_path);
        statusView = findViewById(R.id.launcher_status);
        refreshDetectedGame();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshDetectedGame();
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
        }
    }

    public void onLaunchGpuTraceViewerClick(final View view) {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        startActivityForResult(intent, REQUEST_OPEN_GPU_TRACE_VIEWER);
    }

    public void onLaunchWindowDemoClick(final View view) {
        startActivity(new Intent(this, WindowDemoActivity.class));
    }

    public void onLaunchDetectedGameClick(final View view) {
        if (detectedGamePath == null) {
            return;
        }
        final Intent emulatorIntent = new Intent(this, EmulatorActivity.class);
        emulatorIntent.putExtra(
                WindowedAppActivity.EXTRA_CVARS, buildEmulatorLaunchArguments(detectedGamePath));
        startActivity(emulatorIntent);
    }

    public void onRescanGamesClick(final View view) {
        refreshDetectedGame();
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
        launchArguments.putString("apu", "nop");
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
        detectedGamePath = detectedGame != null ? detectedGame.getAbsolutePath() : null;
        launchGameButton.setEnabled(detectedGamePath != null);

        final StringBuilder status = new StringBuilder();
        if (isProbablyEmulator()) {
            status.append(getString(R.string.launcher_status_emulator_warning)).append('\n');
        }
        if (detectedGamePath != null) {
            status.append(getString(R.string.launcher_status_ready));
            gamePathView.setText(detectedGamePath);
        } else {
            status.append(getString(R.string.launcher_status_missing_game));
            gamePathView.setText(
                    getString(R.string.launcher_game_path_hint, getPreferredImportDirectory()));
        }
        statusView.setText(status.toString().trim());
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
