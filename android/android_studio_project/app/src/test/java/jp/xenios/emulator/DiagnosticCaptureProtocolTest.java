package jp.xenios.emulator;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class DiagnosticCaptureProtocolTest {
    @Test
    public void requestIdIsStableUtcAndFilesystemSafe() {
        final String requestId =
                DiagnosticCaptureProtocol.createRequestId(0L, 123, 456L);

        assertEquals("19700101T000000.000Z-p123-e456", requestId);
        assertTrue(DiagnosticCaptureProtocol.isValidRequestId(requestId));
    }

    @Test
    public void requestIdRejectsTraversalAndMalformedValues() {
        assertFalse(DiagnosticCaptureProtocol.isValidRequestId("../request"));
        assertFalse(DiagnosticCaptureProtocol.isValidRequestId("20260725-p1"));
        assertFalse(DiagnosticCaptureProtocol.isValidRequestId(null));
    }

    @Test
    public void nativeSnapshotFallsBackToEmptyObject() {
        assertEquals("{}", DiagnosticCaptureProtocol.normalizeNativeSnapshot(null));
        assertEquals("{}", DiagnosticCaptureProtocol.normalizeNativeSnapshot("not-json"));
        assertEquals(
                "{\"renderer\":\"vulkan\"}",
                DiagnosticCaptureProtocol.normalizeNativeSnapshot(
                        "  {\"renderer\":\"vulkan\"} \n"));
    }
}
