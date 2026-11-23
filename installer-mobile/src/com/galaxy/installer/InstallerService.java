package com.galaxy.installer;

import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.Environment;
import android.os.IBinder;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Background worker service running in :installer that executes scanning,
 * disc extraction (nodtool), Python asset cooking, and Riivolution XML cooking
 * while publishing atomic status files to <filesDir>/cooker/ for the in-app
 * Comet Observatory Settings UI.
 */
public class InstallerService extends Service {
    private static final String TAG = "GalaxyInstallerSvc";

    public static final String ACTION_SCAN = "scan";
    public static final String ACTION_SELECT_SOURCE = "select_source";
    public static final String ACTION_START_GAME_COOK = "start_game_cook";
    public static final String ACTION_INSPECT_MOD = "inspect_mod";
    public static final String ACTION_START_MOD_COOK = "start_mod_cook";
    public static final String ACTION_CANCEL_MOD = "cancel_mod";
    public static final String ACTION_DELETE_MOD = "delete_mod";

    public static final String EXTRA_ACTION = "action";
    public static final String EXTRA_SOURCE_PATH = "source_path";
    public static final String EXTRA_SOURCE_LABEL = "source_label";
    public static final String EXTRA_MODE = "mode";
    public static final String EXTRA_WITH_MOVIES = "with_movies";
    public static final String EXTRA_PACK_ID = "pack_id";
    public static final String EXTRA_OPTIONS_JSON = "options_json";
    public static final String EXTRA_MOD_ID = "mod_id";

    private static final int MAX_LOG_LINES = 120;

    private final ExecutorService mExecutor = Executors.newSingleThreadExecutor();
    private final ArrayDeque<String> mLogLines = new ArrayDeque<>();
    private final Object mStateLock = new Object();

    private String mState = "idle";
    private String mMode = "game";
    private int mPercent = 0;
    private String mStatus = "Ready to prepare game data.";
    private String mSourcePath = "";
    private String mSourceLabel = "";
    private String mPackId = "";
    private String mPackName = "";
    private int mPpcPatches = 0;
    private String mError = "";

    private String mModStagingPath = "";
    private File mModTempZipDir = null;
    private File mModUriDir = null;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            return START_NOT_STICKY;
        }
        final String action = intent.getStringExtra(EXTRA_ACTION);
        final String srcPath = intent.getStringExtra(EXTRA_SOURCE_PATH);
        final String srcLabel = intent.getStringExtra(EXTRA_SOURCE_LABEL);
        final String mode = intent.getStringExtra(EXTRA_MODE);
        final boolean withMovies = intent.getBooleanExtra(EXTRA_WITH_MOVIES, false);
        final String packId = intent.getStringExtra(EXTRA_PACK_ID);
        final String optionsJson = intent.getStringExtra(EXTRA_OPTIONS_JSON);
        final String modId = intent.getStringExtra(EXTRA_MOD_ID);

        mExecutor.execute(() -> handleAction(
            action, srcPath, srcLabel, mode, withMovies, packId, optionsJson, modId));
        return START_NOT_STICKY;
    }

    private File getCookerDir() {
        File dir = new File(getFilesDir(), "cooker");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    private boolean isStorageGranted() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return true;
    }

    private String sanitizeIniVal(String s) {
        if (s == null) return "";
        return s.replace('\n', ' ').replace('\r', ' ').trim();
    }

    private void writeStatus() {
        synchronized (mStateLock) {
            File dir = getCookerDir();
            File tmp = new File(dir, "status.ini.tmp");
            File dst = new File(dir, "status.ini");
            try (FileWriter fw = new FileWriter(tmp)) {
                fw.write("state=" + sanitizeIniVal(mState) + "\n");
                fw.write("mode=" + sanitizeIniVal(mMode) + "\n");
                fw.write("percent=" + mPercent + "\n");
                fw.write("storage_granted=" + (isStorageGranted() ? "1" : "0") + "\n");
                fw.write("status=" + sanitizeIniVal(mStatus) + "\n");
                fw.write("source_path=" + sanitizeIniVal(mSourcePath) + "\n");
                fw.write("source_label=" + sanitizeIniVal(mSourceLabel) + "\n");
                fw.write("pack_id=" + sanitizeIniVal(mPackId) + "\n");
                fw.write("pack_name=" + sanitizeIniVal(mPackName) + "\n");
                fw.write("ppc_patches=" + mPpcPatches + "\n");
                fw.write("error=" + sanitizeIniVal(mError) + "\n");
            } catch (Exception e) {
                Log.e(TAG, "Failed writing status.ini", e);
                return;
            }
            if (dst.exists()) dst.delete();
            tmp.renameTo(dst);
        }
    }

    private void appendLog(String line) {
        if (line == null || line.trim().isEmpty()) return;
        synchronized (mStateLock) {
            mLogLines.addLast(line.trim());
            while (mLogLines.size() > MAX_LOG_LINES) {
                mLogLines.removeFirst();
            }
            File dir = getCookerDir();
            File tmp = new File(dir, "log.txt.tmp");
            File dst = new File(dir, "log.txt");
            try (FileWriter fw = new FileWriter(tmp)) {
                for (String l : mLogLines) {
                    fw.write(l);
                    fw.write('\n');
                }
            } catch (Exception ignored) {
                return;
            }
            if (dst.exists()) dst.delete();
            tmp.renameTo(dst);
        }
    }

    private void setProgressState(String state, int percent, String status) {
        synchronized (mStateLock) {
            if (state != null) mState = state;
            mPercent = percent;
            if (status != null) mStatus = status;
        }
        writeStatus();
    }

    private void handleAction(
            String action, String srcPath, String srcLabel, String mode,
            boolean withMovies, String packId, String optionsJson, String modId) {
        if (action == null) action = ACTION_SCAN;
        try {
            if (ACTION_SCAN.equals(action)) {
                doScan(mode);
            } else if (ACTION_SELECT_SOURCE.equals(action)) {
                synchronized (mStateLock) {
                    mMode = (mode != null && !mode.isEmpty()) ? mode : "game";
                    mSourcePath = srcPath != null ? srcPath : "";
                    mSourceLabel = (srcLabel != null && !srcLabel.isEmpty()) ? srcLabel : new File(mSourcePath).getName();
                    mState = "idle";
                    mError = "";
                    mStatus = "Selected: " + mSourceLabel;
                }
                appendLog("[Source] " + mSourceLabel + " (" + mSourcePath + ")");
                writeStatus();
            } else if (ACTION_START_GAME_COOK.equals(action)) {
                doGameCook(srcPath, withMovies);
            } else if (ACTION_INSPECT_MOD.equals(action)) {
                doInspectMod(srcPath, srcLabel);
            } else if (ACTION_START_MOD_COOK.equals(action)) {
                doCookMod(packId, optionsJson);
            } else if (ACTION_CANCEL_MOD.equals(action)) {
                cleanModStaging();
                synchronized (mStateLock) {
                    mState = "idle";
                    mPercent = 0;
                    mError = "";
                    mStatus = "Mod setup cancelled.";
                }
                writeStatus();
            } else if (ACTION_DELETE_MOD.equals(action)) {
                doDeleteMod(modId);
            }
        } catch (Throwable t) {
            Log.e(TAG, "Action failed: " + action, t);
            appendLog("[ERROR] " + t.getMessage());
            synchronized (mStateLock) {
                mState = "error";
                mError = t.getMessage() != null ? t.getMessage() : "Unknown error";
                mStatus = "Error: " + mError;
            }
            writeStatus();
        }
    }

    private void doScan(String mode) {
        List<String> lines = new ArrayList<>();
        String autoGamePath = "";
        String autoGameLabel = "";

        // 1. Check existing cooked GalaxyData
        File gd = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);

        List<File> searchDirs = new ArrayList<>();
        if (gd.isDirectory()) searchDirs.add(gd);
        if (dl != null && dl.isDirectory()) searchDirs.add(dl);
        File extRiiv = new File(Environment.getExternalStorageDirectory(), "Riivolution");
        if (extRiiv.isDirectory()) searchDirs.add(extRiiv);
        File gdRiiv = new File(gd, "riivolution");
        if (gdRiiv.isDirectory()) searchDirs.add(gdRiiv);

        for (File dir : searchDirs) {
            if (new File(dir, "riivolution").isDirectory()) {
                lines.add("mod|" + dir.getAbsolutePath() + "|" + dir.getName() + " (XML Folder)");
            }
            File[] children = dir.listFiles();
            if (children == null) continue;
            for (File ch : children) {
                String low = ch.getName().toLowerCase();
                if (ch.isFile()) {
                    if (low.endsWith(".rvz") || low.endsWith(".wbfs") || low.endsWith(".iso")
                            || low.endsWith(".wia") || low.endsWith(".ciso")) {
                        lines.add("game|" + ch.getAbsolutePath() + "|" + ch.getName());
                        if (autoGamePath.isEmpty()) {
                            autoGamePath = ch.getAbsolutePath();
                            autoGameLabel = ch.getName();
                        }
                    } else if (low.endsWith(".zip")) {
                        if (low.contains("riiv") || low.contains("mod") || low.contains("neo")) {
                            lines.add("mod|" + ch.getAbsolutePath() + "|" + ch.getName());
                        } else if (low.contains("galaxy")) {
                            lines.add("game|" + ch.getAbsolutePath() + "|" + ch.getName());
                            if (autoGamePath.isEmpty()) {
                                autoGamePath = ch.getAbsolutePath();
                                autoGameLabel = ch.getName();
                            }
                        } else {
                            lines.add("mod|" + ch.getAbsolutePath() + "|" + ch.getName());
                        }
                    }
                } else if (ch.isDirectory()) {
                    if (new File(ch, "riivolution").isDirectory()) {
                        lines.add("mod|" + ch.getAbsolutePath() + "|" + ch.getName() + " (Folder)");
                    } else if (new File(ch, "sys/fst.bin").isFile() || new File(ch, "DATA/sys/fst.bin").isFile()) {
                        lines.add("game|" + ch.getAbsolutePath() + "|" + ch.getName() + " (Extracted)");
                        if (autoGamePath.isEmpty()) {
                            autoGamePath = ch.getAbsolutePath();
                            autoGameLabel = ch.getName();
                        }
                    }
                }
            }
        }

        File cookerDir = getCookerDir();
        File tmp = new File(cookerDir, "detected.ini.tmp");
        File dst = new File(cookerDir, "detected.ini");
        try (FileWriter fw = new FileWriter(tmp)) {
            for (String l : lines) {
                fw.write(l);
                fw.write('\n');
            }
            if (dst.exists()) dst.delete();
            tmp.renameTo(dst);
        } catch (Exception ignored) {}

        synchronized (mStateLock) {
            if (mode != null && !mode.isEmpty()) mMode = mode;
            if (mSourcePath.isEmpty() && !autoGamePath.isEmpty()) {
                mSourcePath = autoGamePath;
                mSourceLabel = autoGameLabel;
                mStatus = "Auto-detected: " + autoGameLabel;
            } else if ("idle".equals(mState) && mSourcePath.isEmpty()) {
                mStatus = isStorageGranted()
                    ? "Select a Super Mario Galaxy disc or folder."
                    : "Grant All Files Access to begin setup.";
            }
        }
        appendLog("[Scan] Found " + lines.size() + " candidate source(s).");
        writeStatus();
    }

    private void doGameCook(String passedSource, boolean withMovies) throws Exception {
        if (!isStorageGranted()) {
            throw new RuntimeException("All Files Access permission is required.");
        }
        String sourcePath;
        synchronized (mStateLock) {
            sourcePath = (passedSource != null && !passedSource.isEmpty()) ? passedSource : mSourcePath;
            mMode = "game";
            mError = "";
            mState = "running";
            mPercent = 0;
            mStatus = "Preparing cooking engine...";
            if (!sourcePath.isEmpty()) mSourcePath = sourcePath;
        }
        if (sourcePath == null || sourcePath.isEmpty()) {
            throw new RuntimeException("No game disc or folder selected.");
        }
        writeStatus();

        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        if (!galaxyDataDir.exists()) galaxyDataDir.mkdirs();
        final String outputDir = galaxyDataDir.getAbsolutePath();

        appendLog("========================================");
        appendLog("[Pipeline] Cooking to: " + outputDir);
        appendLog("[Source] " + sourcePath);

        CookerEngine.prepareAssets(this, new CookerEngine.CookerCallback() {
            @Override public void onLog(String line) { appendLog(line); }
            @Override public void onProgress(int pct, String msg) {}
            @Override public void onFinished(boolean ok, String msg) {}
        });

        String pythonHome = new File(getFilesDir(), "pythonhome").getAbsolutePath();
        String scriptsDir = new File(getFilesDir(), "scripts").getAbsolutePath();

        File uriImportDir = new File(getFilesDir(), "uri_import");
        File tempExtractionDir = null;

        try {
            File sourceFile = CookerEngine.resolveOrImportSource(
                this, sourcePath, uriImportDir,
                new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int pct, String msg) {
                        setProgressState("running", Math.max(0, pct), msg);
                    }
                    @Override public void onFinished(boolean ok, String msg) {}
                }
            );

            String resolvedPath = sourceFile.getAbsolutePath();
            String cookInputPath = resolvedPath;
            String lower = resolvedPath.toLowerCase();
            boolean isDisc = sourceFile.isFile() && (
                lower.endsWith(".rvz") || lower.endsWith(".wbfs") || lower.endsWith(".iso") ||
                lower.endsWith(".wia") || lower.endsWith(".ciso") || CookerEngine.isWiiDisc(resolvedPath)
            );

            if (isDisc) {
                setProgressState("running", 5, "Extracting Wii disc image...");
                tempExtractionDir = new File(getFilesDir(), "extracted_cache");
                File extractMarker = new File(tempExtractionDir, ".extract_complete");
                String expectedTag = resolvedPath + "|" + sourceFile.length() + "|" + withMovies;
                File fstCheck = new File(tempExtractionDir, "sys/fst.bin");
                if (!fstCheck.exists()) fstCheck = new File(tempExtractionDir, "DATA/sys/fst.bin");

                boolean cacheOk = false;
                if (extractMarker.exists() && fstCheck.exists() && fstCheck.length() > 0) {
                    try (BufferedReader br = new BufferedReader(new FileReader(extractMarker))) {
                        cacheOk = expectedTag.equals(br.readLine());
                    } catch (Exception ignored) {}
                }

                if (cacheOk) {
                    appendLog("[Stage 1/2] Using cached disc extraction.");
                } else {
                    CookerEngine.deleteRecursive(tempExtractionDir);
                    tempExtractionDir.mkdirs();

                    boolean ok = CookerEngine.extractWithNodtool(
                        this, resolvedPath, tempExtractionDir.getAbsolutePath(), withMovies,
                        new CookerEngine.CookerCallback() {
                            @Override public void onLog(String line) { appendLog(line); }
                            @Override public void onProgress(int pct, String msg) {
                                setProgressState("running", pct >= 0 ? (5 + pct / 3) : mPercent, msg);
                            }
                            @Override public void onFinished(boolean ok2, String msg) {}
                        }
                    );

                    if (!ok && (lower.endsWith(".wbfs") || lower.endsWith(".iso"))) {
                        appendLog("[Fallback] Retrying with C++ disc extractor...");
                        ok = (CookerEngine.extractDisc(
                            resolvedPath, tempExtractionDir.getAbsolutePath(), withMovies,
                            new CookerEngine.CookerCallback() {
                                @Override public void onLog(String line) { appendLog(line); }
                                @Override public void onProgress(int pct, String msg) {
                                    setProgressState("running", pct >= 0 ? (5 + pct / 3) : mPercent, msg);
                                }
                                @Override public void onFinished(boolean ok2, String msg) {}
                            }
                        ) == 0);
                    }
                    if (!ok) {
                        throw new RuntimeException("Disc extraction failed. Verify dump and storage space.");
                    }
                    try (FileWriter fw = new FileWriter(extractMarker)) {
                        fw.write(expectedTag);
                    } catch (Exception ignored) {}
                }

                File subData = new File(tempExtractionDir, "DATA");
                if (subData.exists() && new File(subData, "files").exists()) {
                    cookInputPath = subData.getAbsolutePath();
                } else {
                    cookInputPath = tempExtractionDir.getAbsolutePath();
                }
            } else if (sourceFile.isFile() && lower.endsWith(".zip")) {
                setProgressState("running", 5, "Unpacking ZIP archive...");
                tempExtractionDir = new File(getFilesDir(), "zip_cache");
                CookerEngine.deleteRecursive(tempExtractionDir);
                tempExtractionDir.mkdirs();
                boolean unzipped = CookerEngine.extractZip(sourceFile, tempExtractionDir, new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int pct, String msg) {
                        setProgressState("running", 10, msg);
                    }
                    @Override public void onFinished(boolean ok, String msg) {}
                });
                if (!unzipped) throw new RuntimeException("Failed unpacking ZIP archive.");
                cookInputPath = tempExtractionDir.getAbsolutePath();
            }

            setProgressState("running", 15, "Cooking textures, models & audio...");
            int ret = CookerEngine.runCooker(
                pythonHome, scriptsDir, cookInputPath, outputDir, withMovies,
                new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int pct, String msg) {
                        setProgressState("running", pct >= 0 ? pct : mPercent, msg);
                    }
                    @Override public void onFinished(boolean ok, String msg) {}
                }
            );
            if (ret != 0) {
                throw new RuntimeException("Cooker returned exit code " + ret);
            }

            if (tempExtractionDir != null && tempExtractionDir.exists()) {
                CookerEngine.deleteRecursive(tempExtractionDir);
            }
            if (uriImportDir.exists()) {
                CookerEngine.deleteRecursive(uriImportDir);
            }
            setProgressState("done", 100, "Game data cooked successfully!");
            appendLog("[SUCCESS] Ready to play!");
        } catch (Exception e) {
            throw e;
        }
    }

    private void cleanModStaging() {
        if (mModTempZipDir != null && mModTempZipDir.exists()) {
            CookerEngine.deleteRecursive(mModTempZipDir);
        }
        mModTempZipDir = null;
        if (mModUriDir != null && mModUriDir.exists()) {
            CookerEngine.deleteRecursive(mModUriDir);
        }
        mModUriDir = null;
        mModStagingPath = "";
    }

    private void doInspectMod(String srcPath, String srcLabel) throws Exception {
        if (!isStorageGranted()) {
            throw new RuntimeException("All Files Access permission is required.");
        }
        if (srcPath == null || srcPath.isEmpty()) {
            throw new RuntimeException("No mod file or folder selected.");
        }

        cleanModStaging();
        synchronized (mStateLock) {
            mMode = "mod";
            mError = "";
            mSourcePath = srcPath;
            mSourceLabel = (srcLabel != null && !srcLabel.isEmpty()) ? srcLabel : new File(srcPath).getName();
            mState = "inspecting";
            mPercent = 0;
            mStatus = "Inspecting Riivolution pack...";
        }
        writeStatus();

        CookerEngine.prepareAssets(this, new CookerEngine.CookerCallback() {
            @Override public void onLog(String line) { appendLog(line); }
            @Override public void onProgress(int pct, String msg) {}
            @Override public void onFinished(boolean ok, String msg) {}
        });

        String pythonHome = new File(getFilesDir(), "pythonhome").getAbsolutePath();
        String scriptsDir = new File(getFilesDir(), "scripts").getAbsolutePath();

        mModUriDir = new File(getFilesDir(), "mod_uri_import");
        File sourceFile = CookerEngine.resolveOrImportSource(
            this, srcPath, mModUriDir,
            new CookerEngine.CookerCallback() {
                @Override public void onLog(String line) { appendLog(line); }
                @Override public void onProgress(int pct, String msg) {
                    setProgressState("inspecting", Math.max(0, pct), msg);
                }
                @Override public void onFinished(boolean ok, String msg) {}
            }
        );

        String inspectPath = sourceFile.getAbsolutePath();
        if (sourceFile.isFile() && sourceFile.getName().toLowerCase().endsWith(".zip")) {
            setProgressState("inspecting", 15, "Unpacking mod archive...");
            mModTempZipDir = new File(getFilesDir(), "riiv_zip_cache");
            CookerEngine.deleteRecursive(mModTempZipDir);
            mModTempZipDir.mkdirs();
            boolean ok = CookerEngine.extractZip(sourceFile, mModTempZipDir, new CookerEngine.CookerCallback() {
                @Override public void onLog(String line) { appendLog(line); }
                @Override public void onProgress(int pct, String msg) {
                    setProgressState("inspecting", 20, msg);
                }
                @Override public void onFinished(boolean ok2, String msg) {}
            });
            if (!ok) throw new RuntimeException("Failed to unpack Riivolution ZIP.");
            inspectPath = mModTempZipDir.getAbsolutePath();
        }
        mModStagingPath = inspectPath;

        String jsonStr = CookerEngine.inspectRiivolution(pythonHome, scriptsDir, inspectPath);
        JSONObject root = new JSONObject(jsonStr != null ? jsonStr : "{\"packs\":[]}");
        JSONArray packs = root.optJSONArray("packs");
        if (packs == null || packs.length() == 0) {
            cleanModStaging();
            throw new RuntimeException("No riivolution/*.xml or SMG mod folders found.");
        }

        // Write mod_options.ini cleanly for C++ Observatory UI
        File optTmp = new File(getCookerDir(), "mod_options.ini.tmp");
        File optDst = new File(getCookerDir(), "mod_options.ini");
        boolean needsOptionsUi = packs.length() > 1;
        int defaultPackIdx = 0;
        for (int i = 0; i < packs.length(); i++) {
            if (packs.getJSONObject(i).optBoolean("game_compatible", true)) {
                defaultPackIdx = i;
                break;
            }
        }

        try (FileWriter fw = new FileWriter(optTmp)) {
            fw.write("pack_count=" + packs.length() + "\n");
            for (int p = 0; p < packs.length(); p++) {
                JSONObject pack = packs.getJSONObject(p);
                String pid = pack.optString("id", "mod_" + p).replace('|', '_');
                String pname = pack.optString("name", pid).replace('|', ' ').replace('\n', ' ');
                int compat = pack.optBoolean("game_compatible", true) ? 1 : 0;
                int mem = pack.optInt("total_memory_patches", 0);
                fw.write("pack|" + p + "|" + pid + "|" + pname + "|" + compat + "|" + mem + "\n");

                JSONArray opts = pack.optJSONArray("options");
                if (opts != null && opts.length() > 0) {
                    if (p == defaultPackIdx) needsOptionsUi = true;
                    for (int o = 0; o < opts.length(); o++) {
                        JSONObject opt = opts.getJSONObject(o);
                        String oKey = opt.optString("key", "").replace('|', ':');
                        String oName = opt.optString("name", oKey).replace('|', ' ').replace('\n', ' ');
                        int defVal = opt.optInt("default", 0);
                        StringBuilder chSb = new StringBuilder();
                        JSONArray choices = opt.optJSONArray("choices");
                        if (choices != null) {
                            for (int c = 0; c < choices.length(); c++) {
                                if (c > 0) chSb.append(";;");
                                chSb.append(choices.getJSONObject(c).optString("name", "Choice " + c)
                                    .replace(";;", "/").replace('|', ' '));
                            }
                        }
                        fw.write("opt|" + p + "|" + oKey + "|" + oName + "|" + defVal + "|" + chSb + "\n");
                    }
                }
            }
        }
        if (optDst.exists()) optDst.delete();
        optTmp.renameTo(optDst);

        JSONObject pDef = packs.getJSONObject(defaultPackIdx);
        int pMem = pDef.optInt("total_memory_patches", 0);
        if (pMem > 0) needsOptionsUi = true;

        synchronized (mStateLock) {
            mPackId = pDef.optString("id", "");
            mPackName = pDef.optString("name", mPackId);
            mPpcPatches = pMem;
        }

        if (needsOptionsUi) {
            setProgressState("options", 0, "Configure Riivolution options below.");
            appendLog("[Riivolution] Inspect complete: ready for option configuration.");
        } else {
            doCookMod(mPackId, "{}");
        }
    }

    private void doCookMod(String packId, String optionsJson) throws Exception {
        if (!isStorageGranted()) {
            throw new RuntimeException("All Files Access permission is required.");
        }
        if (mModStagingPath == null || mModStagingPath.isEmpty()) {
            // Re-stage if user re-cooked without staging
            throw new RuntimeException("Mod source is no longer staged. Please re-select the mod.");
        }
        synchronized (mStateLock) {
            mMode = "mod";
            mError = "";
            mState = "running";
            mPercent = 0;
            mStatus = "Cooking Riivolution mod assets...";
            if (packId != null && !packId.isEmpty()) mPackId = packId;
        }
        writeStatus();

        String pythonHome = new File(getFilesDir(), "pythonhome").getAbsolutePath();
        String scriptsDir = new File(getFilesDir(), "scripts").getAbsolutePath();
        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        if (!galaxyDataDir.exists()) galaxyDataDir.mkdirs();

        int ret = CookerEngine.runRiivolutionCooker(
            pythonHome, scriptsDir, mModStagingPath, galaxyDataDir.getAbsolutePath(),
            mPackId, (optionsJson != null && !optionsJson.isEmpty()) ? optionsJson : "{}",
            new CookerEngine.CookerCallback() {
                @Override public void onLog(String line) { appendLog(line); }
                @Override public void onProgress(int pct, String msg) {
                    setProgressState("running", pct >= 0 ? pct : mPercent, msg);
                }
                @Override public void onFinished(boolean ok, String msg) {}
            }
        );

        cleanModStaging();
        if (ret != 0) {
            throw new RuntimeException("Riivolution cooker returned error code " + ret);
        }
        setProgressState("done", 100, "Riivolution mod installed & enabled!");
    }

    private void doDeleteMod(String modId) {
        if (modId == null || modId.trim().isEmpty()) return;
        String cleanId = modId.trim();
        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        File modsDir = new File(galaxyDataDir, "mods");
        File target = new File(modsDir, cleanId);
        if (target.isDirectory()) {
            CookerEngine.deleteRecursive(target);
        }
        File cfg = new File(modsDir, "mods_config.ini");
        if (cfg.isFile()) {
            List<String> keep = new ArrayList<>();
            try (BufferedReader br = new BufferedReader(new FileReader(cfg))) {
                String line;
                while ((line = br.readLine()) != null) {
                    String s = line.trim();
                    if (s.isEmpty() || s.startsWith("#")) continue;
                    int bar = s.indexOf('|');
                    if (bar > 0 && cleanId.equals(s.substring(bar + 1).trim())) continue;
                    keep.add(s);
                }
            } catch (Exception ignored) {}
            try (FileWriter fw = new FileWriter(cfg)) {
                fw.write("# GalaxyMobile Riivolution Mods Config\n");
                fw.write("# Order from top to bottom = highest to lowest priority\n");
                for (String k : keep) {
                    fw.write(k + "\n");
                }
            } catch (Exception ignored) {}
        }
        appendLog("[Riivolution] Removed mod: " + cleanId);
        writeStatus();
    }
}
