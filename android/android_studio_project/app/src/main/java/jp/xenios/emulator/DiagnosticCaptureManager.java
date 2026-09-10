package jp.xenios.emulator;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.os.Debug;
import android.os.Process;
import android.os.SystemClock;
import android.util.JsonWriter;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

final class DiagnosticCaptureManager {
    private static final String DIAGNOSTICS_DIRECTORY = "diagnostics";
    private static final String REQUESTS_DIRECTORY = "requests";
    private static final String PROFILE_FILENAME = "xenios_android_profile.txt";

    private DiagnosticCaptureManager() {
    }

    static Result capture(
            final Context context, final String targetPath, final String nativeSnapshot,
            final String saveStatePath) {
        return capture(
                context, targetPath, nativeSnapshot, true, saveStatePath);
    }

    static Result captureReport(
            final Context context, final String targetPath, final String nativeSnapshot) {
        return capture(
                context, targetPath, nativeSnapshot, false, null);
    }

    private static Result capture(
            final Context context, final String targetPath, final String nativeSnapshot,
            final boolean createsSaveState, final String saveStatePath) {
        final long wallClockMillis = System.currentTimeMillis();
        final long elapsedRealtimeMillis = SystemClock.elapsedRealtime();
        final int processId = Process.myPid();
        final String requestId = DiagnosticCaptureProtocol.createRequestId(
                wallClockMillis, processId, elapsedRealtimeMillis);
        Log.i(
                DiagnosticCaptureProtocol.LOG_TAG,
                "XENIOS_DIAGNOSTIC_REQUEST_BEGIN id=" + requestId
                        + " elapsed_ms=" + elapsedRealtimeMillis);

        try {
            final File externalRoot = context.getExternalFilesDir(null);
            if (externalRoot == null) {
                throw new IOException("app external files directory is unavailable");
            }
            final File diagnosticsRoot = new File(externalRoot, DIAGNOSTICS_DIRECTORY);
            final File requestsRoot = new File(diagnosticsRoot, REQUESTS_DIRECTORY);
            final File requestDirectory = new File(requestsRoot, requestId);
            ensureDirectory(requestDirectory);

            writeAtomically(
                    new File(requestDirectory, "request.json"),
                    createRequestJson(
                            context, requestId, wallClockMillis, elapsedRealtimeMillis,
                            processId, targetPath, createsSaveState, saveStatePath));
            writeAtomically(
                    new File(requestDirectory, "app_runtime.json"),
                    createRuntimeJson(context));
            writeAtomically(
                    new File(requestDirectory, "native_snapshot.json"),
                    DiagnosticCaptureProtocol.normalizeNativeSnapshot(nativeSnapshot) + "\n");
            copyProfileIfPresent(context, requestDirectory);
            writeAtomically(
                    new File(requestDirectory, DiagnosticCaptureProtocol.READY_MARKER),
                    requestId + "\n");
            writeAtomically(
                    new File(diagnosticsRoot, DiagnosticCaptureProtocol.LATEST_REQUEST_FILE),
                    requestId + "\n");

            Log.i(
                    DiagnosticCaptureProtocol.LOG_TAG,
                    "XENIOS_DIAGNOSTIC_REQUEST_READY id=" + requestId
                            + " path=" + requestDirectory.getAbsolutePath());
            return Result.success(requestId, requestDirectory);
        } catch (final Exception e) {
            Log.e(
                    DiagnosticCaptureProtocol.LOG_TAG,
                    "XENIOS_DIAGNOSTIC_REQUEST_FAILED id=" + requestId,
                    e);
            return Result.failure(requestId, e.getMessage());
        }
    }

    private static String createRequestJson(
            final Context context, final String requestId, final long wallClockMillis,
            final long elapsedRealtimeMillis, final int processId, final String targetPath,
            final boolean createsSaveState, final String saveStatePath)
            throws IOException {
        final StringWriter output = new StringWriter();
        try (JsonWriter json = new JsonWriter(output)) {
            json.setIndent("  ");
            json.beginObject();
            json.name("protocol_version").value(DiagnosticCaptureProtocol.VERSION);
            json.name("request_id").value(requestId);
            json.name("created_at_utc").value(formatUtc(wallClockMillis));
            json.name("wall_clock_epoch_ms").value(wallClockMillis);
            json.name("elapsed_realtime_ms").value(elapsedRealtimeMillis);
            json.name("package_name").value(context.getPackageName());
            json.name("process_id").value(processId);
            json.name("debuggable").value(
                    (context.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0);
            json.name("guest_target").value(targetPath != null ? targetPath : "");
            json.name("app_runtime_file").value("app_runtime.json");
            json.name("native_snapshot_file").value("native_snapshot.json");
            json.name("host_collector_required").value(true);
            json.name("renderdoc_capture_requested").value(false);
            json.name("creates_emulator_save_state").value(createsSaveState);
            json.name("emulator_save_state_path");
            if (createsSaveState) {
                json.value(saveStatePath);
            } else {
                json.nullValue();
            }
            json.name("notes").value(createsSaveState
                    ? "A checksummed in-process diagnostic save was created before this marker. "
                            + "The host collector may now gather screenshot, bounded logcat, "
                            + "and process state."
                    : "No emulator save state was created. The host collector may gather "
                            + "screenshot, bounded logcat, and process state.");
            json.endObject();
        }
        return output.toString() + "\n";
    }

    private static String createRuntimeJson(final Context context) throws IOException {
        final Runtime runtime = Runtime.getRuntime();
        final ActivityManager activityManager =
                (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        final ActivityManager.MemoryInfo systemMemory = new ActivityManager.MemoryInfo();
        if (activityManager != null) {
            activityManager.getMemoryInfo(systemMemory);
        }
        final Debug.MemoryInfo processMemory = new Debug.MemoryInfo();
        Debug.getMemoryInfo(processMemory);

        int importance = -1;
        if (activityManager != null) {
            final List<ActivityManager.RunningAppProcessInfo> processes =
                    activityManager.getRunningAppProcesses();
            if (processes != null) {
                for (final ActivityManager.RunningAppProcessInfo process : processes) {
                    if (process.pid == Process.myPid()) {
                        importance = process.importance;
                        break;
                    }
                }
            }
        }

        final StringWriter output = new StringWriter();
        try (JsonWriter json = new JsonWriter(output)) {
            json.setIndent("  ");
            json.beginObject();
            json.name("device");
            json.beginObject();
            json.name("manufacturer").value(Build.MANUFACTURER);
            json.name("model").value(Build.MODEL);
            json.name("device").value(Build.DEVICE);
            json.name("hardware").value(Build.HARDWARE);
            json.name("android_sdk").value(Build.VERSION.SDK_INT);
            json.name("fingerprint").value(Build.FINGERPRINT);
            json.name("supported_abis");
            json.beginArray();
            for (final String abi : Build.SUPPORTED_ABIS) {
                json.value(abi);
            }
            json.endArray();
            json.endObject();

            json.name("process");
            json.beginObject();
            json.name("pid").value(Process.myPid());
            json.name("uid").value(Process.myUid());
            json.name("importance").value(importance);
            json.name("native_heap_allocated_bytes").value(Debug.getNativeHeapAllocatedSize());
            json.name("native_heap_size_bytes").value(Debug.getNativeHeapSize());
            json.name("native_heap_free_bytes").value(Debug.getNativeHeapFreeSize());
            json.name("total_pss_kb").value(processMemory.getTotalPss());
            json.name("total_private_dirty_kb").value(processMemory.getTotalPrivateDirty());
            json.name("total_shared_dirty_kb").value(processMemory.getTotalSharedDirty());
            json.endObject();

            json.name("java_runtime");
            json.beginObject();
            json.name("max_memory_bytes").value(runtime.maxMemory());
            json.name("total_memory_bytes").value(runtime.totalMemory());
            json.name("free_memory_bytes").value(runtime.freeMemory());
            json.name("available_processors").value(runtime.availableProcessors());
            json.endObject();

            json.name("system_memory");
            json.beginObject();
            json.name("available_bytes").value(systemMemory.availMem);
            json.name("total_bytes").value(systemMemory.totalMem);
            json.name("threshold_bytes").value(systemMemory.threshold);
            json.name("low_memory").value(systemMemory.lowMemory);
            json.endObject();
            json.endObject();
        }
        return output.toString() + "\n";
    }

    private static void copyProfileIfPresent(
            final Context context, final File requestDirectory) throws IOException {
        final File internalProfile = new File(context.getFilesDir(), PROFILE_FILENAME);
        if (internalProfile.isFile()) {
            copyFile(internalProfile, new File(requestDirectory, "active_profile.txt"));
        }
        final File externalRoot = context.getExternalFilesDir(null);
        if (externalRoot != null) {
            final File externalProfile = new File(externalRoot, PROFILE_FILENAME);
            if (externalProfile.isFile()) {
                copyFile(
                        externalProfile,
                        new File(requestDirectory, "external_profile_override.txt"));
            }
        }
    }

    private static void copyFile(final File source, final File destination) throws IOException {
        try (InputStream input = new FileInputStream(source);
             OutputStream output = new FileOutputStream(destination)) {
            final byte[] buffer = new byte[8192];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read != 0) {
                    output.write(buffer, 0, read);
                }
            }
            output.flush();
        }
    }

    private static void ensureDirectory(final File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("unable to create " + directory.getAbsolutePath());
        }
    }

    private static void writeAtomically(final File destination, final String contents)
            throws IOException {
        final File parent = destination.getParentFile();
        if (parent == null) {
            throw new IOException("destination has no parent");
        }
        ensureDirectory(parent);
        final File temporary = new File(parent, destination.getName() + ".tmp");
        try (FileWriter writer = new FileWriter(temporary, false)) {
            writer.write(contents);
            writer.flush();
        }
        if (destination.exists() && !destination.delete()) {
            throw new IOException("unable to replace " + destination.getAbsolutePath());
        }
        if (!temporary.renameTo(destination)) {
            throw new IOException("unable to publish " + destination.getAbsolutePath());
        }
    }

    private static String formatUtc(final long wallClockMillis) {
        final SimpleDateFormat format =
                new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.ROOT);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date(wallClockMillis));
    }

    static final class Result {
        private final String requestId;
        private final File requestDirectory;
        private final String errorMessage;

        private Result(
                final String requestId, final File requestDirectory, final String errorMessage) {
            this.requestId = requestId;
            this.requestDirectory = requestDirectory;
            this.errorMessage = errorMessage;
        }

        static Result success(final String requestId, final File requestDirectory) {
            return new Result(requestId, requestDirectory, null);
        }

        static Result failure(final String requestId, final String errorMessage) {
            return new Result(
                    requestId,
                    null,
                    errorMessage != null ? errorMessage : "unknown error");
        }

        boolean isSuccess() {
            return requestDirectory != null;
        }

        String getRequestId() {
            return requestId;
        }

        String getErrorMessage() {
            return errorMessage;
        }
    }
}
