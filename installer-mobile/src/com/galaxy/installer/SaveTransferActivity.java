package com.galaxy.installer;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.provider.OpenableColumns;
import android.util.Log;
import android.widget.Toast;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Translucent helper activity running in :installer that handles Android SAF document
 * picking, save export (.zip backup & share), and validated save import (.zip or raw GameData.bin).
 */
public class SaveTransferActivity extends Activity {
    private static final String TAG = "SaveTransferActivity";

    public static final String EXTRA_MODE = "mode";
    public static final String EXTRA_SAVE_ROOT = "save_root";
    public static final String EXTRA_GAME_BOOTED = "game_booted";
    public static final String EXTRA_IS_PLUS = "is_plus";
    public static final String EXTRA_MAIN_PID = "main_pid";

    public static final String MODE_EXPORT = "export";
    public static final String MODE_IMPORT = "import";

    private static final String SMG_SAVE_SUBPATH = "title/00010000/524d4750/data";
    private static final int REQ_CREATE_EXPORT_ZIP = 2001;
    private static final int REQ_PICK_IMPORT_FILE = 2002;
    private static final int REQ_SHARE_EXPORT_ZIP = 2003;
    private static final int MAX_PICK_BYTES = 16 * 1024 * 1024;
    private static final int MAX_SAVE_ENTRY_BYTES = 2 * 1024 * 1024;

    private String mMode = MODE_EXPORT;
    private String mSaveRoot = "";
    private boolean mGameBooted = false;
    private boolean mIsPlus = false;
    private int mMainPid = -1;
    private boolean mFlowStarted = false;

    private static final class ParsedSavePayload {
        byte[] gameData;
        byte[] banner;
        String sourceLabel;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Intent intent = getIntent();
        if (intent != null) {
            String mode = intent.getStringExtra(EXTRA_MODE);
            if (mode != null) mMode = mode;
            String root = intent.getStringExtra(EXTRA_SAVE_ROOT);
            if (root != null) mSaveRoot = root;
            mGameBooted = intent.getBooleanExtra(EXTRA_GAME_BOOTED, false);
            mIsPlus = intent.getBooleanExtra(EXTRA_IS_PLUS, false);
            mMainPid = intent.getIntExtra(EXTRA_MAIN_PID, -1);
        }

        if (mSaveRoot.isEmpty()) {
            mSaveRoot = new File(getFilesDir(), "nand").getAbsolutePath();
        }

        if (savedInstanceState != null) {
            mFlowStarted = savedInstanceState.getBoolean("flow_started", false);
        }

        if (!mFlowStarted) {
            mFlowStarted = true;
            if (MODE_IMPORT.equals(mMode)) {
                startImportPicker();
            } else {
                startExportDialog();
            }
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putBoolean("flow_started", mFlowStarted);
    }

    private File getSaveDataDir() {
        return new File(mSaveRoot, SMG_SAVE_SUBPATH);
    }

    private File resolvePrimarySaveFile() {
        File dir = getSaveDataDir();
        File primary = new File(dir, "GameData.bin");
        if (primary.exists() && primary.length() >= 0x140) {
            return primary;
        }
        File backup = new File(dir, "GameData.bin.bak");
        if (backup.exists() && backup.length() >= 0x140) {
            return backup;
        }
        return null;
    }

    private String makeExportZipFilename() {
        String prefix = mIsPlus ? "GalaxyMobilePlus_Save_" : "GalaxyMobile_Save_";
        String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        return prefix + stamp + ".zip";
    }

    private void startExportDialog() {
        File gameData = resolvePrimarySaveFile();
        if (gameData == null) {
            Toast.makeText(this, "No save data found yet. Save in-game first before exporting.", Toast.LENGTH_LONG).show();
            finish();
            return;
        }

        String sizeText = String.format(Locale.US, "%.1f KB", gameData.length() / 1024.0);
        String title = mIsPlus ? "Export GalaxyMobile+ Save" : "Export GalaxyMobile Save";

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage("Your Super Mario Galaxy save (" + sizeText + ") can be saved as a portable .zip backup or shared directly to another device.")
                .setPositiveButton("Save to Files (.zip)", (dialog, which) -> launchExportDocumentPicker())
                .setNeutralButton("Share Backup", (dialog, which) -> shareExportZipAsync())
                .setNegativeButton("Cancel", (dialog, which) -> finish())
                .setOnCancelListener(dialog -> finish())
                .show();
    }

    private void launchExportDocumentPicker() {
        try {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            intent.putExtra(Intent.EXTRA_TITLE, makeExportZipFilename());
            startActivityForResult(intent, REQ_CREATE_EXPORT_ZIP);
        } catch (Exception e) {
            Log.e(TAG, "Failed to open export document picker", e);
            Toast.makeText(this, "Cannot open file saver: " + e.getMessage(), Toast.LENGTH_LONG).show();
            finish();
        }
    }

    private void writeSaveZip(OutputStream rawOut) throws IOException {
        File saveDir = getSaveDataDir();
        File primary = resolvePrimarySaveFile();
        if (primary == null) {
            throw new IOException("GameData.bin not found");
        }

        try (ZipOutputStream zos = new ZipOutputStream(rawOut)) {
            writeFileEntry(zos, SMG_SAVE_SUBPATH + "/GameData.bin", primary);
            File banner = new File(saveDir, "banner.bin");
            if (banner.exists() && banner.length() > 0) {
                writeFileEntry(zos, SMG_SAVE_SUBPATH + "/banner.bin", banner);
            }
        }
    }

    private static void writeFileEntry(ZipOutputStream zos, String entryName, File file) throws IOException {
        ZipEntry entry = new ZipEntry(entryName);
        entry.setTime(file.lastModified() > 0 ? file.lastModified() : System.currentTimeMillis());
        zos.putNextEntry(entry);
        try (FileInputStream fis = new FileInputStream(file)) {
            byte[] buf = new byte[16384];
            int n;
            while ((n = fis.read(buf)) > 0) {
                zos.write(buf, 0, n);
            }
        }
        zos.closeEntry();
    }

    private void shareExportZipAsync() {
        new Thread(() -> {
            try {
                File cacheDir = getExternalCacheDir();
                if (cacheDir == null) cacheDir = getCacheDir();
                if (!cacheDir.exists()) cacheDir.mkdirs();

                String zipName = makeExportZipFilename();
                File outZip = new File(cacheDir, zipName);
                try (FileOutputStream fos = new FileOutputStream(outZip)) {
                    writeSaveZip(fos);
                }

                runOnUiThread(() -> {
                    try {
                        Uri shareUri = Uri.parse("content://" + getPackageName() + ".fileprovider/" + outZip.getName());
                        Intent shareIntent = new Intent(Intent.ACTION_SEND);
                        shareIntent.setType("application/zip");
                        shareIntent.putExtra(Intent.EXTRA_STREAM, shareUri);
                        shareIntent.putExtra(Intent.EXTRA_SUBJECT, mIsPlus ? "GalaxyMobile+ Save Backup" : "GalaxyMobile Save Backup");
                        shareIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

                        Intent chooser = Intent.createChooser(shareIntent, "Share Save Backup (.zip)");
                        chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        startActivityForResult(chooser, REQ_SHARE_EXPORT_ZIP);
                    } catch (Exception e) {
                        Log.e(TAG, "Failed to share save zip", e);
                        Toast.makeText(this, "Failed to share backup: " + e.getMessage(), Toast.LENGTH_LONG).show();
                        finish();
                    }
                });
            } catch (Exception e) {
                Log.e(TAG, "Failed to create shareable save zip", e);
                runOnUiThread(() -> {
                    Toast.makeText(this, "Export failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    finish();
                });
            }
        }).start();
    }

    private void startImportPicker() {
        try {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, REQ_PICK_IMPORT_FILE);
        } catch (Exception e) {
            Log.e(TAG, "Failed to open import file picker", e);
            Toast.makeText(this, "Cannot open file picker: " + e.getMessage(), Toast.LENGTH_LONG).show();
            finish();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQ_CREATE_EXPORT_ZIP) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                finish();
                return;
            }
            final Uri destUri = data.getData();
            new Thread(() -> {
                try {
                    OutputStream os = getContentResolver().openOutputStream(destUri, "wt");
                    if (os == null) {
                        os = getContentResolver().openOutputStream(destUri);
                    }
                    if (os == null) {
                        throw new IOException("Unable to open output destination");
                    }
                    try (OutputStream managedOut = os) {
                        writeSaveZip(managedOut);
                    }
                    runOnUiThread(() -> {
                        Toast.makeText(this, "Save backup exported successfully!", Toast.LENGTH_LONG).show();
                        finish();
                    });
                } catch (Exception e) {
                    Log.e(TAG, "Failed to write export zip to URI", e);
                    runOnUiThread(() -> {
                        Toast.makeText(this, "Failed to export save: " + e.getMessage(), Toast.LENGTH_LONG).show();
                        finish();
                    });
                }
            }).start();
        } else if (requestCode == REQ_PICK_IMPORT_FILE) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                finish();
                return;
            }
            final Uri srcUri = data.getData();
            importInspectUriAsync(srcUri);
        } else if (requestCode == REQ_SHARE_EXPORT_ZIP) {
            finish();
        }
    }

    private void importInspectUriAsync(Uri srcUri) {
        new Thread(() -> {
            try {
                byte[] raw = readAllLimited(srcUri, MAX_PICK_BYTES);
                String displayName = queryDisplayName(srcUri);
                ParsedSavePayload parsed = parseSavePayload(raw, displayName);

                if (parsed == null || parsed.gameData == null) {
                    runOnUiThread(() -> {
                        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                                .setTitle("Invalid Save File")
                                .setMessage("The selected file is not a valid Super Mario Galaxy save backup (.zip or GameData.bin).")
                                .setPositiveButton("OK", (d, w) -> finish())
                                .setOnCancelListener(d -> finish())
                                .show();
                    });
                    return;
                }

                runOnUiThread(() -> confirmAndApplyImport(parsed));
            } catch (Exception e) {
                Log.e(TAG, "Failed inspecting selected save file", e);
                runOnUiThread(() -> {
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Import Failed")
                            .setMessage("Could not read selected file: " + e.getMessage())
                            .setPositiveButton("OK", (d, w) -> finish())
                            .setOnCancelListener(d -> finish())
                            .show();
                });
            }
        }).start();
    }

    private void confirmAndApplyImport(ParsedSavePayload parsed) {
        String contents = "GameData.bin (" + String.format(Locale.US, "%.1f KB", parsed.gameData.length / 1024.0) + ")";
        if (parsed.banner != null) {
            contents += " + banner.bin";
        }

        String msg = "Detected valid save: " + contents;
        if (parsed.sourceLabel != null && !parsed.sourceLabel.isEmpty()) {
            msg = "File: " + parsed.sourceLabel + "\n" + msg;
        }
        msg += "\n\nImporting will overwrite your current save data on this device";
        if (mGameBooted) {
            msg += " and restart the game to load it.";
        } else {
            msg += ".";
        }

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Import Save Data?")
                .setMessage(msg)
                .setPositiveButton(mGameBooted ? "Import & Restart" : "Import Save", (d, w) -> applyImportedSave(parsed))
                .setNegativeButton("Cancel", (d, w) -> finish())
                .setOnCancelListener(d -> finish())
                .show();
    }

    private void applyImportedSave(ParsedSavePayload parsed) {
        new Thread(() -> {
            try {
                File dir = getSaveDataDir();
                if (!dir.exists() && !dir.mkdirs()) {
                    throw new IOException("Failed to create save directory");
                }

                File primary = new File(dir, "GameData.bin");
                File safetyBackup = new File(dir, "GameData.bin.pre_import");
                if (primary.exists() && primary.length() >= 0x140) {
                    writeBytesAtomic(safetyBackup, readFileBytes(primary));
                }

                // Write both GameData.bin and GameData.bin.bak so NAND auto-recovery matches.
                writeBytesAtomic(primary, parsed.gameData);
                writeBytesAtomic(new File(dir, "GameData.bin.bak"), parsed.gameData);

                File bannerFile = new File(dir, "banner.bin");
                if (parsed.banner != null && parsed.banner.length > 0) {
                    writeBytesAtomic(bannerFile, parsed.banner);
                } else if (bannerFile.exists()) {
                    bannerFile.delete();
                }

                runOnUiThread(() -> {
                    if (mGameBooted) {
                        Toast.makeText(this, "Save imported! Restarting...", Toast.LENGTH_SHORT).show();
                        restartMainGameProcess();
                    } else {
                        Toast.makeText(this, "Save imported successfully!", Toast.LENGTH_LONG).show();
                        finish();
                    }
                });
            } catch (Exception e) {
                Log.e(TAG, "Failed writing imported save", e);
                runOnUiThread(() -> {
                    Toast.makeText(this, "Failed to save imported data: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    finish();
                });
            }
        }).start();
    }

    private void restartMainGameProcess() {
        // Kill the paused game process first so singleTask/static native state clears cleanly,
        // then relaunch the main launcher activity from this foreground activity.
        if (mMainPid > 0 && mMainPid != Process.myPid()) {
            try {
                Process.killProcess(mMainPid);
            } catch (Exception e) {
                Log.w(TAG, "Could not kill main pid " + mMainPid, e);
            }
        }
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            try {
                Intent launchIntent = getPackageManager().getLaunchIntentForPackage(getPackageName());
                if (launchIntent != null) {
                    launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                    startActivity(launchIntent);
                }
            } catch (Exception e) {
                Log.e(TAG, "Failed to relaunch game activity", e);
            }
            finish();
        }, 120);
    }

    private byte[] readAllLimited(Uri uri, int maxBytes) throws IOException {
        try (InputStream is = getContentResolver().openInputStream(uri)) {
            if (is == null) {
                throw new IOException("Cannot open selected file");
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int total = 0;
            int n;
            while ((n = is.read(buf)) > 0) {
                total += n;
                if (total > maxBytes) {
                    throw new IOException("File is too large for a Super Mario Galaxy save");
                }
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }

    private static byte[] readLimitedStream(InputStream is, int maxBytes) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int total = 0;
        int n;
        while ((n = is.read(buf)) > 0) {
            total += n;
            if (total > maxBytes) {
                throw new IOException("Entry exceeds max save size");
            }
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static byte[] readFileBytes(File file) throws IOException {
        try (FileInputStream fis = new FileInputStream(file)) {
            return readLimitedStream(fis, MAX_SAVE_ENTRY_BYTES);
        }
    }

    private static void writeBytesAtomic(File dest, byte[] data) throws IOException {
        File tmp = new File(dest.getParentFile(), dest.getName() + ".tmp");
        try (FileOutputStream fos = new FileOutputStream(tmp)) {
            fos.write(data);
            fos.flush();
            fos.getFD().sync();
        }
        if (dest.exists() && !dest.delete()) {
            throw new IOException("Could not replace " + dest.getName());
        }
        if (!tmp.renameTo(dest)) {
            throw new IOException("Could not move temp file into " + dest.getName());
        }
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) {
                    return c.getString(idx);
                }
            }
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "";
    }

    private static ParsedSavePayload parseSavePayload(byte[] raw, String displayName) throws IOException {
        ParsedSavePayload out = new ParsedSavePayload();
        out.sourceLabel = displayName;

        if (raw.length >= 4 && raw[0] == 0x50 && raw[1] == 0x4B && raw[2] == 0x03 && raw[3] == 0x04) {
            byte[] exactGameData = null;
            byte[] fallbackGameData = null;
            byte[] exactBanner = null;
            byte[] fallbackBanner = null;

            try (ZipInputStream zis = new ZipInputStream(new ByteArrayInputStream(raw))) {
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    if (entry.isDirectory()) continue;
                    String name = entry.getName().replace('\\', '/');
                    if (name.contains("..") || name.startsWith("__MACOSX/") || name.contains("/__MACOSX/")) {
                        continue;
                    }
                    int slash = name.lastIndexOf('/');
                    String base = (slash >= 0) ? name.substring(slash + 1) : name;

                    if ("gamedata.bin".equalsIgnoreCase(base)) {
                        byte[] data = readLimitedStream(zis, MAX_SAVE_ENTRY_BYTES);
                        if (isValidGameDataBin(data)) {
                            if (name.toLowerCase(Locale.US).endsWith(SMG_SAVE_SUBPATH + "/gamedata.bin")) {
                                exactGameData = data;
                            } else if (fallbackGameData == null) {
                                fallbackGameData = data;
                            }
                        }
                    } else if ("banner.bin".equalsIgnoreCase(base)) {
                        byte[] data = readLimitedStream(zis, MAX_SAVE_ENTRY_BYTES);
                        if (data.length > 0) {
                            if (name.toLowerCase(Locale.US).endsWith(SMG_SAVE_SUBPATH + "/banner.bin")) {
                                exactBanner = data;
                            } else if (fallbackBanner == null) {
                                fallbackBanner = data;
                            }
                        }
                    }
                }
            }

            out.gameData = (exactGameData != null) ? exactGameData : fallbackGameData;
            out.banner = (exactBanner != null) ? exactBanner : fallbackBanner;
            return out.gameData != null ? out : null;
        }

        if (isValidGameDataBin(raw)) {
            out.gameData = raw;
            return out;
        }

        return null;
    }

    private static boolean isValidGameDataBin(byte[] data) {
        if (data == null || data.length < 0x140 || data.length > MAX_SAVE_ENTRY_BYTES) {
            return false;
        }
        return checkSaveHeader(data, true) || checkSaveHeader(data, false);
    }

    private static boolean checkSaveHeader(byte[] d, boolean littleEndian) {
        long version = readU32(d, 4, littleEndian);
        long infoNum = readU32(d, 8, littleEndian);
        long fileSize = readU32(d, 12, littleEndian);
        if (version != 2 || infoNum == 0 || infoNum >= 24) {
            return false;
        }
        long minHeaderSize = 16L + infoNum * 16L;
        return fileSize >= minHeaderSize && fileSize <= d.length && fileSize <= 0x10000L;
    }

    private static long readU32(byte[] d, int off, boolean littleEndian) {
        if (littleEndian) {
            return ((long) (d[off] & 0xFF)) |
                   (((long) (d[off + 1] & 0xFF)) << 8) |
                   (((long) (d[off + 2] & 0xFF)) << 16) |
                   (((long) (d[off + 3] & 0xFF)) << 24);
        }
        return (((long) (d[off] & 0xFF)) << 24) |
               (((long) (d[off + 1] & 0xFF)) << 16) |
               (((long) (d[off + 2] & 0xFF)) << 8) |
               ((long) (d[off + 3] & 0xFF));
    }
}
