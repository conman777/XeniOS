package jp.xenios.emulator;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.regex.Pattern;

final class DiagnosticCaptureProtocol {
    static final int VERSION = 1;
    static final String LOG_TAG = "XeniOSDiagnostics";
    static final String READY_MARKER = "READY";
    static final String COLLECTED_MARKER = "HOST_COLLECTED";
    static final String LATEST_REQUEST_FILE = "LATEST_REQUEST";

    private static final Pattern REQUEST_ID_PATTERN =
            Pattern.compile("[0-9]{8}T[0-9]{6}\\.[0-9]{3}Z-p[0-9]+-e[0-9]+");

    private DiagnosticCaptureProtocol() {
    }

    static String createRequestId(
            final long wallClockMillis, final int processId, final long elapsedRealtimeMillis) {
        final SimpleDateFormat format =
                new SimpleDateFormat("yyyyMMdd'T'HHmmss.SSS'Z'", Locale.ROOT);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date(wallClockMillis))
                + "-p" + processId
                + "-e" + elapsedRealtimeMillis;
    }

    static boolean isValidRequestId(final String requestId) {
        return requestId != null && REQUEST_ID_PATTERN.matcher(requestId).matches();
    }

    static String normalizeNativeSnapshot(final String snapshot) {
        if (snapshot == null) {
            return "{}";
        }
        final String trimmed = snapshot.trim();
        if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) {
            return "{}";
        }
        return trimmed;
    }
}
