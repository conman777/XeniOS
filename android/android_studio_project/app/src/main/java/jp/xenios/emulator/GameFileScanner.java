package jp.xenios.emulator;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

final class GameFileScanner {
    private static final String[] GAME_EXTENSIONS = {".iso", ".xex", ".zar"};
    private static final String[] SKIPPED_DIRECTORY_NAMES = {
        "cache", "cache0", "cache1", "code_cache", "content", "diagnostics",
        "modules", "shaders", "update_empty"
    };
    private static final int MAX_SCAN_DEPTH = 8;

    private GameFileScanner() {}

    static File findNewestGame(final List<File> roots) {
        final List<File> candidates = new ArrayList<>();
        final Set<String> visitedDirectories = new HashSet<>();
        for (final File root : roots) {
            collectGameFiles(root, candidates, visitedDirectories, 0);
        }
        if (candidates.isEmpty()) {
            return null;
        }
        Collections.sort(candidates, new Comparator<File>() {
            @Override
            public int compare(final File left, final File right) {
                final int modifiedCompare = Long.compare(right.lastModified(), left.lastModified());
                if (modifiedCompare != 0) {
                    return modifiedCompare;
                }
                return left.getAbsolutePath().compareToIgnoreCase(right.getAbsolutePath());
            }
        });
        return candidates.get(0);
    }

    static boolean isGameFile(final File file) {
        final String lowerName = file.getName().toLowerCase(Locale.ROOT);
        for (final String extension : GAME_EXTENSIONS) {
            if (lowerName.endsWith(extension)) {
                return true;
            }
        }
        return false;
    }

    private static void collectGameFiles(
            final File root,
            final List<File> results,
            final Set<String> visitedDirectories,
            final int depth) {
        if (root == null || depth > MAX_SCAN_DEPTH || !root.exists() || !root.isDirectory()) {
            return;
        }
        final String canonicalPath = canonicalPath(root);
        if (!visitedDirectories.add(canonicalPath)) {
            return;
        }
        final File[] children = root.listFiles();
        if (children == null) {
            return;
        }
        for (final File child : children) {
            if (child.isDirectory()) {
                if (!isSkippedDirectory(child)) {
                    collectGameFiles(child, results, visitedDirectories, depth + 1);
                }
            } else if (isGameFile(child)) {
                results.add(child);
            }
        }
    }

    private static boolean isSkippedDirectory(final File directory) {
        final String name = directory.getName().toLowerCase(Locale.ROOT);
        for (final String skippedName : SKIPPED_DIRECTORY_NAMES) {
            if (name.equals(skippedName)) {
                return true;
            }
        }
        return false;
    }

    private static String canonicalPath(final File file) {
        try {
            return file.getCanonicalPath();
        } catch (final IOException e) {
            return file.getAbsolutePath();
        }
    }
}
