package com.galaxy.installer;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class CookerEngine {
    private static final String TAG = "CookerEngine";

    static {
        try {
            System.loadLibrary("python3.10");
        } catch (Throwable t) {
            Log.w(TAG, "Optional direct load of libpython3.10.so: " + t.getMessage());
        }
        try {
            System.loadLibrary("installer");
        } catch (Throwable e) {
            Log.e(TAG, "Failed to load native library: libinstaller.so", e);
        }
    }

    public interface CookerCallback {
        void onLog(String line);
        void onProgress(int percent, String message);
        void onFinished(boolean success, String message);
    }

    public static native int runCooker(String pythonHome, String scriptsDir, String inputDir, String outputDir, boolean withMovies, CookerCallback callback);
    public static native boolean isWiiDisc(String filePath);
    public static native int extractDisc(String imagePath, String outDir, boolean includeMovies, CookerCallback callback);

    public static boolean extractZip(File zipFile, File outDir, CookerCallback callback) {
        if (callback != null) callback.onLog("[Extractor] Unpacking ZIP archive: " + zipFile.getName() + "...");
        byte[] buffer = new byte[65536];
        try (InputStream is = new java.io.FileInputStream(zipFile);
             ZipInputStream zis = new ZipInputStream(is)) {
            ZipEntry entry;
            int count = 0;
            while ((entry = zis.getNextEntry()) != null) {
                File outFile = new File(outDir, entry.getName());
                if (entry.isDirectory()) {
                    outFile.mkdirs();
                } else {
                    outFile.getParentFile().mkdirs();
                    try (FileOutputStream fos = new FileOutputStream(outFile)) {
                        int len;
                        while ((len = zis.read(buffer)) > 0) {
                            fos.write(buffer, 0, len);
                        }
                    }
                }
                zis.closeEntry();
                count++;
                if (count % 100 == 0 && callback != null) {
                    callback.onProgress(-1, "Unpacked " + count + " files...");
                }
            }
            if (callback != null) callback.onLog("[Extractor] Finished unpacking ZIP archive (" + count + " items).");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to unpack zip: " + zipFile.getAbsolutePath(), e);
            if (callback != null) callback.onLog("[Error] Failed to unpack zip: " + e.getMessage());
            return false;
        }
    }

    public static void deleteRecursive(File fileOrDirectory) {
        if (fileOrDirectory == null || !fileOrDirectory.exists()) return;
        if (fileOrDirectory.isDirectory()) {
            File[] children = fileOrDirectory.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursive(child);
                }
            }
        }
        fileOrDirectory.delete();
    }

    public static boolean extractWithNodtool(Context context, String imagePath, String outDir, boolean includeMovies, CookerCallback callback) {
        File libDir = new File(context.getApplicationInfo().nativeLibraryDir);
        File nodtool = new File(libDir, "libnodtool.so");
        if (!nodtool.exists()) {
            nodtool = new File(context.getFilesDir(), "bin/nodtool");
        }
        nodtool.setExecutable(true, false);

        if (callback != null) callback.onLog("[Extractor] Invoking native disc extractor on: " + new File(imagePath).getName());

        try {
            ProcessBuilder pb = new ProcessBuilder(
                nodtool.getAbsolutePath(), "extract", "-p", "data", imagePath, outDir
            );
            pb.redirectErrorStream(true);
            Process proc = pb.start();

            try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(proc.getInputStream()))) {
                String line;
                int count = 0;
                while ((line = reader.readLine()) != null) {
                    if (line.startsWith("Extracting ")) {
                        count++;
                        if (count % 35 == 0 && callback != null) {
                            String rel = line.replace("Extracting ", "").trim();
                            callback.onProgress(-1, "Extracting: " + rel);
                        }
                    } else if (callback != null && !line.trim().isEmpty()) {
                        callback.onLog(line.trim());
                    }
                }
            }

            int exitCode = proc.waitFor();
            if (exitCode != 0) {
                if (callback != null) callback.onLog("[Error] Disc extractor failed with exit code: " + exitCode);
                return false;
            }

            if (!includeMovies) {
                File movieDir = new File(outDir, "files/MovieData");
                if (movieDir.exists()) {
                    if (callback != null) callback.onLog("[Extractor] Omitting cutscenes (saving 2.3 GB space)...");
                    deleteRecursive(movieDir);
                }
            }

            if (callback != null) callback.onLog("[Extractor] Disc extraction successfully finished.");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Nodtool extraction error", e);
            if (callback != null) callback.onLog("[Error] Failed to extract disc: " + e.getMessage());
            return false;
        }
    }

    public static void prepareAssets(Context context, CookerCallback callback) throws Exception {
        File filesDir = context.getFilesDir();
        File pythonHomeDir = new File(filesDir, "pythonhome");
        File scriptsDir = new File(filesDir, "scripts");

        if (!pythonHomeDir.exists()) pythonHomeDir.mkdirs();
        if (!scriptsDir.exists()) scriptsDir.mkdirs();

        // Unpack pythonhome.zip if not already extracted (force re-extract for ARM64)
        File stdlibMarker = new File(pythonHomeDir, ".extracted_arm64_v1");
        if (!stdlibMarker.exists()) {
            if (callback != null) callback.onLog("[Setup] Unpacking embedded Python standard library (ARM64)...");
            deleteRecursive(pythonHomeDir);
            pythonHomeDir.mkdirs();
            try (InputStream is = context.getAssets().open("pythonhome.zip");
                 ZipInputStream zis = new ZipInputStream(is)) {
                ZipEntry entry;
                byte[] buffer = new byte[8192];
                while ((entry = zis.getNextEntry()) != null) {
                    File outFile = new File(pythonHomeDir, entry.getName());
                    if (entry.isDirectory()) {
                        outFile.mkdirs();
                    } else {
                        outFile.getParentFile().mkdirs();
                        try (FileOutputStream fos = new FileOutputStream(outFile)) {
                            int len;
                            while ((len = zis.read(buffer)) > 0) {
                                fos.write(buffer, 0, len);
                            }
                        }
                        if (outFile.getName().endsWith(".so")) {
                            outFile.setReadable(true, false);
                            outFile.setExecutable(true, false);
                        }
                    }
                    zis.closeEntry();
                }
            }
            stdlibMarker.createNewFile();
            if (callback != null) callback.onLog("[Setup] Python runtime ready.");
        }

        // Copy Python cooking scripts from assets/scripts/ into files/scripts/
        deleteRecursive(new File(scriptsDir, "__pycache__"));
        String[] scriptAssets = context.getAssets().list("scripts");
        if (scriptAssets != null) {
            byte[] buffer = new byte[8192];
            for (String file : scriptAssets) {
                if (!file.endsWith(".py")) continue;
                File dest = new File(scriptsDir, file);
                try (InputStream in = context.getAssets().open("scripts/" + file);
                     OutputStream out = new FileOutputStream(dest)) {
                    int len;
                    while ((len = in.read(buffer)) > 0) {
                        out.write(buffer, 0, len);
                    }
                }
            }
        }
    }

    /**
     * Resolves a picked document or tree URI to a verified existing local filesystem path
     * whenever possible, falling back to {@code uri.toString()} rather than fabricating a path.
     */
    public static String resolveSourceFromUri(Context context, Uri uri) {
        if (uri == null) return "";
        if ("file".equalsIgnoreCase(uri.getScheme())) {
            return uri.getPath() != null ? uri.getPath() : "";
        }

        ContentResolver cr = context.getContentResolver();
        String docId = extractDocumentId(context, uri);
        if (docId != null && !docId.isEmpty()) {
            String fromDoc = resolveFromDocumentId(cr, docId);
            if (fromDoc != null) return fromDoc;
        }

        String directData = queryExistingDataPath(cr, uri, null, null, -1);
        if (directData != null) return directData;

        String displayName = queryDisplayName(cr, uri);
        long size = queryOpenableSize(cr, uri);
        if (displayName != null && !displayName.isEmpty()) {
            String byMediaName = queryMediaStoreByName(cr, displayName, size);
            if (byMediaName != null) return byMediaName;

            File[] candidateDirs = new File[] {
                new File(Environment.getExternalStorageDirectory(), "GalaxyData"),
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
            };
            for (File dir : candidateDirs) {
                File found = findFileInDir(dir, displayName, size, 4);
                if (found != null) return found.getAbsolutePath();
            }
        }

        return uri.toString();
    }

    /** Returns the user-facing filename for a picked URI, or {@code null} if unavailable. */
    public static String getDisplayName(Context context, Uri uri) {
        if (uri == null) return null;
        if ("file".equalsIgnoreCase(uri.getScheme())) {
            String p = uri.getPath();
            return p != null ? new File(p).getName() : null;
        }
        String name = queryDisplayName(context.getContentResolver(), uri);
        if (name != null && !name.isEmpty()) return name;
        String last = uri.getLastPathSegment();
        if (last == null) return null;
        int colon = last.lastIndexOf(':');
        if (colon >= 0 && colon + 1 < last.length()) last = last.substring(colon + 1);
        int slash = last.lastIndexOf('/');
        return (slash >= 0 && slash + 1 < last.length()) ? last.substring(slash + 1) : last;
    }

    /**
     * Validates a filesystem source path or copies an unresolved {@code content://} document
     * stream into {@code importDir} so native extractors and Python can read it by path.
     */
    public static File resolveOrImportSource(Context context, String sourcePath, File importDir, CookerCallback callback) throws IOException {
        if (sourcePath == null || sourcePath.trim().isEmpty()) {
            throw new IllegalArgumentException("No game disc or extracted folder selected.");
        }
        if (!sourcePath.startsWith("content://")) {
            File f = new File(sourcePath);
            if (!f.exists()) {
                throw new IOException("Selected source does not exist: " + sourcePath + ". Please re-select your game file or folder.");
            }
            return f;
        }

        Uri uri = Uri.parse(sourcePath);
        if (DocumentsContract.isTreeUri(uri) && !DocumentsContract.isDocumentUri(context, uri)) {
            throw new IOException("Could not resolve local path for the selected folder. Please select it from Internal Storage or pick the disc file directly.");
        }

        String name = getDisplayName(context, uri);
        if (name == null || name.trim().isEmpty()) name = "game_source";
        String safeName = name.replace('/', '_').replace('\\', '_');

        deleteRecursive(importDir);
        if (!importDir.mkdirs() && !importDir.isDirectory()) {
            throw new IOException("Failed to prepare temporary import directory.");
        }
        File outFile = new File(importDir, safeName);
        if (callback != null) {
            callback.onLog("[Stage 1/2] Importing selected file (" + safeName + ") from system picker...");
            callback.onProgress(-1, "Importing " + safeName + "...");
        }

        long totalSize = queryOpenableSize(context.getContentResolver(), uri);
        byte[] buf = new byte[131072];
        long copied = 0;
        long nextProgressAt = 16L * 1024L * 1024L;
        try (InputStream in = context.getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(outFile)) {
            if (in == null) {
                throw new IOException("Unable to open selected file stream from system picker.");
            }
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                copied += n;
                if (callback != null && (copied >= nextProgressAt || (totalSize > 0 && copied == totalSize))) {
                    int pct = totalSize > 0 ? (int) Math.min(100L, (copied * 100L) / totalSize) : -1;
                    long mb = copied / (1024L * 1024L);
                    callback.onProgress(pct, "Importing " + safeName + " (" + mb + " MB)...");
                    nextProgressAt += 16L * 1024L * 1024L;
                }
            }
        }
        if (copied == 0) {
            throw new IOException("Selected content URI produced an empty file.");
        }
        return outFile;
    }

    private static String extractDocumentId(Context context, Uri uri) {
        try {
            if (DocumentsContract.isDocumentUri(context, uri)) {
                return DocumentsContract.getDocumentId(uri);
            }
            if (DocumentsContract.isTreeUri(uri)) {
                return DocumentsContract.getTreeDocumentId(uri);
            }
        } catch (Exception ignored) {}
        String p = uri.getPath();
        if (p == null) return null;
        if (p.startsWith("/document/")) return p.substring("/document/".length());
        if (p.startsWith("/tree/")) {
            int docIdx = p.indexOf("/document/");
            if (docIdx >= 0) return p.substring(docIdx + "/document/".length());
            return p.substring("/tree/".length());
        }
        return null;
    }

    private static String resolveFromDocumentId(ContentResolver cr, String docId) {
        if (docId.startsWith("raw:")) {
            String raw = docId.substring(4);
            if (!raw.isEmpty() && new File(raw).exists()) return raw;
        }
        if (docId.matches("\\d+")) {
            return queryMediaStoreById(cr, docId);
        }
        int colon = docId.indexOf(':');
        if (colon <= 0) return null;

        String prefix = docId.substring(0, colon);
        String rest = docId.substring(colon + 1);
        if ("primary".equalsIgnoreCase(prefix)) {
            File f = new File(Environment.getExternalStorageDirectory(), rest);
            if (f.exists()) return f.getAbsolutePath();
        } else if ("home".equalsIgnoreCase(prefix)) {
            File f = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS), rest);
            if (f.exists()) return f.getAbsolutePath();
        } else if (("msf".equalsIgnoreCase(prefix) || "document".equalsIgnoreCase(prefix)
                || "image".equalsIgnoreCase(prefix) || "video".equalsIgnoreCase(prefix)
                || "audio".equalsIgnoreCase(prefix)) && rest.matches("\\d+")) {
            String mediaPath = queryMediaStoreById(cr, rest);
            if (mediaPath != null) return mediaPath;
        } else {
            File vol = new File("/storage/" + prefix, rest);
            if (vol.exists()) return vol.getAbsolutePath();
        }
        File dlRel = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), rest);
        return (!rest.isEmpty() && dlRel.exists()) ? dlRel.getAbsolutePath() : null;
    }

    private static String queryMediaStoreById(ContentResolver cr, String mediaId) {
        String sel = MediaStore.MediaColumns._ID + "=?";
        String[] args = new String[] { mediaId };
        String path = queryExistingDataPath(cr, MediaStore.Files.getContentUri("external"), sel, args, -1);
        if (path != null) return path;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            return queryExistingDataPath(cr, MediaStore.Downloads.EXTERNAL_CONTENT_URI, sel, args, -1);
        }
        return null;
    }

    private static String queryMediaStoreByName(ContentResolver cr, String displayName, long expectedSize) {
        String sel = MediaStore.MediaColumns.DISPLAY_NAME + "=?";
        String[] args = new String[] { displayName };
        String path = queryExistingDataPath(cr, MediaStore.Files.getContentUri("external"), sel, args, expectedSize);
        if (path != null || expectedSize <= 0) return path;
        return queryExistingDataPath(cr, MediaStore.Files.getContentUri("external"), sel, args, -1);
    }

    @SuppressWarnings("deprecation")
    private static String queryExistingDataPath(ContentResolver cr, Uri tableUri, String sel, String[] args, long expectedSize) {
        String[] proj = expectedSize > 0
            ? new String[] { MediaStore.MediaColumns.DATA, MediaStore.MediaColumns.SIZE }
            : new String[] { MediaStore.MediaColumns.DATA };
        try (Cursor cursor = cr.query(tableUri, proj, sel, args, null)) {
            if (cursor == null) return null;
            int dataCol = cursor.getColumnIndex(MediaStore.MediaColumns.DATA);
            int sizeCol = expectedSize > 0 ? cursor.getColumnIndex(MediaStore.MediaColumns.SIZE) : -1;
            if (dataCol < 0) return null;
            while (cursor.moveToNext()) {
                if (sizeCol >= 0 && cursor.getLong(sizeCol) != expectedSize) continue;
                String p = cursor.getString(dataCol);
                if (p != null && !p.isEmpty() && new File(p).exists()) return p;
            }
        } catch (Exception ignored) {}
        return null;
    }

    private static String queryDisplayName(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) return c.getString(idx);
            }
        } catch (Exception ignored) {}
        return null;
    }

    private static long queryOpenableSize(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.SIZE);
                if (idx >= 0 && !c.isNull(idx)) return c.getLong(idx);
            }
        } catch (Exception ignored) {}
        return -1;
    }

    private static File findFileInDir(File dir, String name, long expectedSize, int maxDepth) {
        if (dir == null || !dir.isDirectory() || maxDepth < 0) return null;
        File direct = new File(dir, name);
        if (direct.isFile() && (expectedSize <= 0 || direct.length() == expectedSize)) return direct;
        if (maxDepth == 0) return null;
        File[] children = dir.listFiles();
        if (children == null) return null;
        File fallback = direct.isFile() ? direct : null;
        for (File child : children) {
            if (!child.isDirectory()) continue;
            File match = findFileInDir(child, name, expectedSize, maxDepth - 1);
            if (match != null) {
                if (expectedSize <= 0 || match.length() == expectedSize) return match;
                if (fallback == null) fallback = match;
            }
        }
        return fallback;
    }
}
