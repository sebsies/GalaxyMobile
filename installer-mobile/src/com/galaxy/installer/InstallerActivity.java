package com.galaxy.installer;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.method.ScrollingMovementMethod;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class InstallerActivity extends Activity {
    private static final String TAG = "GalaxyInstaller";
    private static final int REQ_PICK_DISC = 1001;
    private static final int REQ_PICK_FOLDER = 1002;
    private static final int REQ_PICK_MOD_FILE = 1003;
    private static final int REQ_PICK_MOD_FOLDER = 1004;

    private TextView txtSourcePath;
    private CheckBox chkIncludeMovies;
    private Button btnSelectDisc;
    private Button btnSelectFolder;
    private Button btnAutoDetect;
    private Button btnInstallMod;
    private Button btnManageMods;
    private Button btnStartCooking;
    private Button btnLaunchGame;
    private Button btnBackToGame;
    private ProgressBar progressBar;
    private TextView txtProgressStatus;
    private TextView txtLogOutput;

    private static final int MAX_LOG_LINES = 250;
    private final ArrayDeque<String> mLogLines = new ArrayDeque<>();
    private boolean mLogUpdatePending = false;

    private String mSelectedSourcePath = "";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mIsCooking = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            setContentView(R.layout.activity_main);

            txtSourcePath = findViewById(R.id.txtSourcePath);
            chkIncludeMovies = findViewById(R.id.chkIncludeMovies);
            btnSelectDisc = findViewById(R.id.btnSelectDisc);
            btnSelectFolder = findViewById(R.id.btnSelectFolder);
            btnAutoDetect = findViewById(R.id.btnAutoDetect);
            btnInstallMod = findViewById(R.id.btnInstallMod);
            btnManageMods = findViewById(R.id.btnManageMods);
            btnStartCooking = findViewById(R.id.btnStartCooking);
            btnLaunchGame = findViewById(R.id.btnLaunchGame);
            btnBackToGame = findViewById(R.id.btnBackToGame);
            progressBar = findViewById(R.id.progressBar);
            txtProgressStatus = findViewById(R.id.txtProgressStatus);
            txtLogOutput = findViewById(R.id.txtLogOutput);

            if (txtLogOutput != null) {
                txtLogOutput.setMovementMethod(new ScrollingMovementMethod());
            }

            checkStoragePermissions();
            autoDetectSource();

            if (btnSelectDisc != null) btnSelectDisc.setOnClickListener(v -> openFilePicker());
            if (btnSelectFolder != null) btnSelectFolder.setOnClickListener(v -> openFolderPicker());
            if (btnAutoDetect != null) btnAutoDetect.setOnClickListener(v -> autoDetectSource());
            if (btnInstallMod != null) btnInstallMod.setOnClickListener(v -> openModSourceChooser());
            if (btnManageMods != null) btnManageMods.setOnClickListener(v -> showManageModsDialog());

            if (getIntent() != null && "riivolution".equals(getIntent().getStringExtra("mode"))) {
                mHandler.postDelayed(this::openModSourceChooser, 250);
            }

            if (btnStartCooking != null) {
                btnStartCooking.setOnClickListener(v -> {
                    if (mSelectedSourcePath.isEmpty()) {
                        Toast.makeText(this, "Please select a game disc or extracted folder first", Toast.LENGTH_SHORT).show();
                        return;
                    }
                    startCookingProcess();
                });
            }

            if (btnBackToGame != null) {
                btnBackToGame.setOnClickListener(v -> returnToGame());
            }
            if (btnLaunchGame != null) {
                btnLaunchGame.setOnClickListener(v -> returnToGame());
            }
        } catch (Throwable t) {
            Log.e(TAG, "Exception during InstallerActivity onCreate", t);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mIsCooking && mSelectedSourcePath.isEmpty()) {
            autoDetectSource();
        }
    }

    private void returnToGame() {
        finish();
    }

    private void checkStoragePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                appendLog("[Permission] Requesting All Files Access for reading game dumps...");
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                try {
                    startActivity(intent);
                } catch (Exception e) {
                    Intent generic = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                    startActivity(generic);
                }
            }
        }
    }

    private void autoDetectSource() {
        appendLog("[Scan] Checking for GalaxyData folder and game dumps...");

        // 1. Check if /sdcard/GalaxyData already exists with ready game data
        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        if (galaxyDataDir.exists()) {
            File fstCheck = new File(galaxyDataDir, "sys/fst.bin");
            File filesCheck = new File(galaxyDataDir, "files");
            if (fstCheck.exists() && filesCheck.exists()) {
                appendLog("[Scan] Found ready game data in " + galaxyDataDir.getAbsolutePath() + "!");
                mHandler.post(() -> {
                    btnLaunchGame.setVisibility(View.VISIBLE);
                    btnLaunchGame.setText("Return to Game 🚀");
                    txtProgressStatus.setText("Game data ready in /sdcard/GalaxyData!");
                });
            }
            // Check for game dumps inside GalaxyData
            File[] gFiles = galaxyDataDir.listFiles();
            if (gFiles != null) {
                for (File f : gFiles) {
                    String name = f.getName().toLowerCase();
                    if (name.endsWith(".rvz") || name.endsWith(".wbfs") || name.endsWith(".iso") || name.endsWith(".zip")) {
                        selectSource(f.getAbsolutePath(), "Auto-detected disc in GalaxyData: " + f.getName());
                        return;
                    }
                }
            }
        }

        // 2. Scan Downloads folder
        File downloadDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (downloadDir != null && downloadDir.exists()) {
            File[] files = downloadDir.listFiles();
            if (files != null) {
                // Priority 1: .rvz, .wbfs, or .iso files
                for (File f : files) {
                    String name = f.getName().toLowerCase();
                    if (name.endsWith(".rvz") || name.endsWith(".wbfs") || name.endsWith(".iso")) {
                        selectSource(f.getAbsolutePath(), "Auto-detected disc: " + f.getName());
                        return;
                    }
                }
                // Priority 2: .zip files
                for (File f : files) {
                    String name = f.getName().toLowerCase();
                    if (name.contains("galaxy") && name.endsWith(".zip")) {
                        selectSource(f.getAbsolutePath(), "Auto-detected archive: " + f.getName());
                        return;
                    }
                }
                // Priority 3: extracted directories
                for (File f : files) {
                    if (f.isDirectory()) {
                        String name = f.getName().toLowerCase();
                        if (name.equals("extracted") || name.equals("data") || name.contains("galaxy")) {
                            File testFiles = new File(f, "files");
                            File testData = new File(f, "DATA");
                            if (testFiles.exists() || testData.exists()) {
                                selectSource(f.getAbsolutePath(), "Auto-detected extracted folder: " + f.getName());
                                return;
                            }
                        }
                    }
                }
            }
        }

        // Check fallback root paths
        String[] fallbackCandidates = {
            "/sdcard/GalaxyData/Super Mario Galaxy.rvz",
            "/sdcard/GalaxyData/Super Mario Galaxy.wbfs",
            "/sdcard/GalaxyData/Super Mario Galaxy.iso",
            "/sdcard/Download/Super Mario Galaxy.rvz",
            "/sdcard/Download/Super Mario Galaxy.wbfs",
            "/sdcard/Download/Super Mario Galaxy.iso",
            "/sdcard/Download/extracted",
            "/sdcard/Download/DATA",
            "/sdcard/GalaxyQuest/extracted",
            "/sdcard/extracted"
        };
        for (String c : fallbackCandidates) {
            File f = new File(c);
            if (f.exists()) {
                selectSource(c, "Auto-detected: " + c);
                return;
            }
        }

        appendLog("[Scan] No disc dump found automatically. Please select your file manually.");
    }

    private void selectSource(String path, String label) {
        mSelectedSourcePath = path;
        txtSourcePath.setText(label);
        appendLog("[Source] " + label + " (" + path + ")");
    }

    private void openFilePicker() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        try {
            startActivityForResult(Intent.createChooser(intent, "Select Super Mario Galaxy (.rvz / .wbfs / .iso / .zip)"), REQ_PICK_DISC);
        } catch (Exception e) {
            Toast.makeText(this, "Failed to open file picker: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void openFolderPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQ_PICK_FOLDER);
        } catch (Exception e) {
            Toast.makeText(this, "Failed to open folder picker: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;

        Uri uri = data.getData();
        if (uri == null) return;

        try {
            if ((data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
        } catch (Exception ignored) {}

        String resolvedPath = CookerEngine.resolveSourceFromUri(this, uri);
        String displayName = CookerEngine.getDisplayName(this, uri);
        String chosen = (resolvedPath != null && !resolvedPath.isEmpty()) ? resolvedPath : uri.toString();

        if (requestCode == REQ_PICK_MOD_FILE || requestCode == REQ_PICK_MOD_FOLDER) {
            beginRiivolutionInspectAndInstall(chosen);
            return;
        }

        if (resolvedPath != null && !resolvedPath.isEmpty()) {
            String labelName = (displayName != null && !displayName.isEmpty())
                ? displayName
                : new File(resolvedPath).getName();
            selectSource(resolvedPath, "Selected: " + labelName);
        } else {
            selectSource(uri.toString(), "Selected: " + (displayName != null ? displayName : uri.getLastPathSegment()));
        }
    }

    private void startCookingProcess() {
        if (mIsCooking) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
            appendLog("[Permission] All Files Access is required to read your dump and write GalaxyData.");
            Toast.makeText(this, "Please allow 'All files access' first, then tap Start Cooking again.", Toast.LENGTH_LONG).show();
            checkStoragePermissions();
            return;
        }
        mIsCooking = true;

        btnStartCooking.setEnabled(false);
        btnSelectDisc.setEnabled(false);
        btnSelectFolder.setEnabled(false);
        btnAutoDetect.setEnabled(false);
        if (btnInstallMod != null) btnInstallMod.setEnabled(false);
        if (btnManageMods != null) btnManageMods.setEnabled(false);
        progressBar.setVisibility(View.VISIBLE);
        txtProgressStatus.setText(R.string.status_cooking);

        final String sourcePath = mSelectedSourcePath;
        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        if (!galaxyDataDir.exists()) {
            boolean created = galaxyDataDir.mkdirs();
            appendLog("[Directory] Created GalaxyData directory: " + galaxyDataDir.getAbsolutePath());
        } else {
            appendLog("[Directory] Using existing GalaxyData directory: " + galaxyDataDir.getAbsolutePath());
        }
        final String outputDir = galaxyDataDir.getAbsolutePath();
        final boolean withMovies = chkIncludeMovies.isChecked();

        appendLog("==============================================");
        appendLog("[Pipeline] Starting PC-less on-device preparation");
        appendLog("[Pipeline] Source: " + sourcePath);
        appendLog("[Pipeline] Target: " + outputDir);
        appendLog("==============================================");

        new Thread(() -> {
            File tempExtractionDir = null;
            File uriImportDir = null;
            try {
                // Step 1: Ensure Python runtime and cooker scripts are unpacked
                CookerEngine.prepareAssets(InstallerActivity.this, new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int percent, String message) {}
                    @Override public void onFinished(boolean success, String message) {}
                });

                String pythonHome = new File(getFilesDir(), "pythonhome").getAbsolutePath();
                String scriptsDir = new File(getFilesDir(), "scripts").getAbsolutePath();

                uriImportDir = new File(getFilesDir(), "uri_import");
                File sourceFile = CookerEngine.resolveOrImportSource(
                    InstallerActivity.this,
                    sourcePath,
                    uriImportDir,
                    new CookerEngine.CookerCallback() {
                        @Override public void onLog(String line) { appendLog(line); }
                        @Override public void onProgress(int percent, String message) {
                            mHandler.post(() -> {
                                if (percent >= 0) {
                                    progressBar.setIndeterminate(false);
                                    progressBar.setProgress(percent);
                                }
                                if (message != null) txtProgressStatus.setText(message);
                            });
                        }
                        @Override public void onFinished(boolean success, String message) {}
                    }
                );
                String resolvedSourcePath = sourceFile.getAbsolutePath();
                String cookInputPath = resolvedSourcePath;

                // Step 2: Handle Wii Disc Image (.rvz / .wbfs / .iso / .wia / .ciso) extraction
                String lower = resolvedSourcePath.toLowerCase();
                boolean isDisc = sourceFile.isFile() && (
                    lower.endsWith(".rvz") || lower.endsWith(".wbfs") || lower.endsWith(".iso") ||
                    lower.endsWith(".wia") || lower.endsWith(".ciso") || CookerEngine.isWiiDisc(resolvedSourcePath)
                );

                if (isDisc) {
                    appendLog("[Stage 1/2] Native Wii Disc detected (" + sourceFile.getName() + ").");
                    appendLog("[Stage 1/2] Decrypting and extracting game data on-device...");
                    mHandler.post(() -> txtProgressStatus.setText("Extracting Wii disc image..."));

                    tempExtractionDir = new File(getFilesDir(), "extracted_cache");
                    File extractMarker = new File(tempExtractionDir, ".extract_complete");
                    String expectedCacheTag = resolvedSourcePath + "|" + sourceFile.length() + "|" + withMovies;
                    File fstCheck = new File(tempExtractionDir, "sys/fst.bin");
                    if (!fstCheck.exists()) {
                        fstCheck = new File(tempExtractionDir, "DATA/sys/fst.bin");
                    }
                    boolean cacheMatches = false;
                    if (extractMarker.exists() && fstCheck.exists() && fstCheck.length() > 0) {
                        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(extractMarker))) {
                            cacheMatches = expectedCacheTag.equals(br.readLine());
                        } catch (Exception ignored) {}
                    }
                    boolean extractSuccess = true;
                    if (cacheMatches) {
                        appendLog("[Stage 1/2] Valid disc cache found from previous extraction. Skipping re-extract.");
                    } else {
                        CookerEngine.deleteRecursive(tempExtractionDir);
                        tempExtractionDir.mkdirs();

                        extractSuccess = CookerEngine.extractWithNodtool(
                            InstallerActivity.this,
                            resolvedSourcePath,
                            tempExtractionDir.getAbsolutePath(),
                            withMovies,
                            new CookerEngine.CookerCallback() {
                                @Override public void onLog(String line) { appendLog(line); }
                                @Override
                                public void onProgress(int percent, String message) {
                                    mHandler.post(() -> {
                                        if (percent >= 0) {
                                            progressBar.setIndeterminate(false);
                                            progressBar.setProgress(percent);
                                        }
                                        if (message != null) {
                                            txtProgressStatus.setText(message);
                                        }
                                    });
                                }
                                @Override public void onFinished(boolean success, String message) {}
                            }
                        );

                        if (!extractSuccess && (lower.endsWith(".wbfs") || lower.endsWith(".iso"))) {
                            appendLog("[Fallback] Retrying with secondary native C++ decryptor...");
                            int extractRet = CookerEngine.extractDisc(
                                resolvedSourcePath,
                                tempExtractionDir.getAbsolutePath(),
                                withMovies,
                                new CookerEngine.CookerCallback() {
                                    @Override public void onLog(String line) { appendLog(line); }
                                    @Override public void onProgress(int percent, String message) {
                                        mHandler.post(() -> {
                                            if (percent >= 0) {
                                                progressBar.setIndeterminate(false);
                                                progressBar.setProgress(percent);
                                            }
                                            if (message != null) txtProgressStatus.setText(message);
                                        });
                                    }
                                    @Override public void onFinished(boolean success, String message) {}
                                }
                            );
                            extractSuccess = (extractRet == 0);
                        }
                        if (extractSuccess) {
                            try (java.io.FileWriter fw = new java.io.FileWriter(extractMarker)) {
                                fw.write(expectedCacheTag);
                            } catch (Exception ignored) {}
                        }
                    }

                    if (!extractSuccess) {
                        throw new RuntimeException("Disc extraction failed. Ensure dump is valid and has sufficient storage space.");
                    }

                    // Find extracted data folder
                    File subData = new File(tempExtractionDir, "DATA");
                    File subFiles = new File(tempExtractionDir, "files");
                    if (subData.exists() && new File(subData, "files").exists()) {
                        cookInputPath = subData.getAbsolutePath();
                    } else if (subFiles.exists()) {
                        cookInputPath = tempExtractionDir.getAbsolutePath();
                    } else {
                        cookInputPath = tempExtractionDir.getAbsolutePath();
                    }
                    appendLog("[Stage 1/2] Extraction finished. Input folder ready: " + cookInputPath);

                } else if (sourceFile.isFile() && lower.endsWith(".zip")) {
                    appendLog("[Stage 1/2] ZIP archive detected. Unpacking...");
                    mHandler.post(() -> txtProgressStatus.setText("Unpacking ZIP archive..."));

                    tempExtractionDir = new File(getFilesDir(), "zip_cache");
                    CookerEngine.deleteRecursive(tempExtractionDir);
                    tempExtractionDir.mkdirs();

                    boolean unzipped = CookerEngine.extractZip(sourceFile, tempExtractionDir, new CookerEngine.CookerCallback() {
                        @Override public void onLog(String line) { appendLog(line); }
                        @Override public void onProgress(int percent, String message) {
                            mHandler.post(() -> {
                                if (message != null) txtProgressStatus.setText(message);
                            });
                        }
                        @Override public void onFinished(boolean success, String message) {}
                    });

                    if (!unzipped) {
                        throw new RuntimeException("Failed to unpack ZIP archive.");
                    }
                    cookInputPath = tempExtractionDir.getAbsolutePath();
                }

                // Step 3: Run the asset converter (cook.py)
                appendLog("[Stage 2/2] Running asset cooker engine (textures, models, audio)...");
                mHandler.post(() -> {
                    progressBar.setIndeterminate(true);
                    txtProgressStatus.setText("Cooking game assets...");
                });

                File outFolder = new File(outputDir);
                if (!outFolder.exists()) {
                    outFolder.mkdirs();
                }

                int ret = CookerEngine.runCooker(
                    pythonHome, scriptsDir, cookInputPath, outputDir, withMovies,
                    new CookerEngine.CookerCallback() {
                        @Override public void onLog(String line) { appendLog(line); }
                        @Override
                        public void onProgress(int percent, String message) {
                            mHandler.post(() -> {
                                if (percent >= 0) {
                                    progressBar.setIndeterminate(false);
                                    progressBar.setProgress(percent);
                                }
                                if (message != null) txtProgressStatus.setText(message);
                            });
                        }
                        @Override public void onFinished(boolean success, String message) {
                            mHandler.post(() -> onCookingCompleted(success, message));
                        }
                    }
                );

                if (ret != 0) {
                    mHandler.post(() -> onCookingCompleted(false, "Cooker returned error code: " + ret));
                } else {
                    // Clean up temporary caches on success to free device storage
                    if ((tempExtractionDir != null && tempExtractionDir.exists())
                            || (uriImportDir != null && uriImportDir.exists())) {
                        appendLog("[Clean] Freeing temporary cache storage...");
                        if (tempExtractionDir != null) CookerEngine.deleteRecursive(tempExtractionDir);
                        if (uriImportDir != null) CookerEngine.deleteRecursive(uriImportDir);
                    }
                    mHandler.post(() -> onCookingCompleted(true, "All game assets cooked successfully!"));
                }

            } catch (Exception e) {
                Log.e(TAG, "Cooking failed with exception", e);
                appendLog("[ERROR] " + e.getMessage());
                mHandler.post(() -> onCookingCompleted(false, e.getMessage()));
            }
        }).start();
    }

    private void onCookingCompleted(boolean success, String message) {
        mIsCooking = false;
        progressBar.setVisibility(View.GONE);
        btnStartCooking.setEnabled(true);
        btnSelectDisc.setEnabled(true);
        btnSelectFolder.setEnabled(true);
        btnAutoDetect.setEnabled(true);
        if (btnInstallMod != null) btnInstallMod.setEnabled(true);
        if (btnManageMods != null) btnManageMods.setEnabled(true);

        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        File coinCheck = new File(galaxyDataDir, "files/ObjectData/Coin.arc");
        File fstCheck = new File(galaxyDataDir, "sys/fst.bin");
        boolean ready = fstCheck.exists() && fstCheck.length() > 0 && coinCheck.exists() && coinCheck.length() > 0;

        if (success || ready) {
            txtProgressStatus.setText(R.string.status_done);
            appendLog("[SUCCESS] " + (message != null ? message : "Conversion complete!"));
            btnLaunchGame.setVisibility(View.VISIBLE);
            btnLaunchGame.setText("Return to Game 🚀");
            Toast.makeText(this, "Game data ready! Return to GalaxyMobile to play.", Toast.LENGTH_LONG).show();
        } else {
            txtProgressStatus.setText(R.string.status_error);
            appendLog("[FAILURE] " + (message != null ? message : "Error during conversion."));
            Toast.makeText(this, "Preparation failed. Check terminal log.", Toast.LENGTH_LONG).show();
        }
        flushLog();
    }

    private void appendLog(String line) {
        if (line == null) return;
        mHandler.post(() -> {
            mLogLines.addLast(line);
            while (mLogLines.size() > MAX_LOG_LINES) {
                mLogLines.removeFirst();
            }
            if (!mLogUpdatePending) {
                mLogUpdatePending = true;
                mHandler.postDelayed(() -> {
                    mLogUpdatePending = false;
                    flushLog();
                }, 40);
            }
        });
    }

    private void flushLog() {
        if (txtLogOutput != null) {
            StringBuilder sb = new StringBuilder(mLogLines.size() * 64);
            for (String l : mLogLines) {
                sb.append(l).append('\n');
            }
            txtLogOutput.setText(sb.toString());
            final int scrollAmount = txtLogOutput.getLayout() != null ?
                    txtLogOutput.getLayout().getLineTop(txtLogOutput.getLineCount()) - txtLogOutput.getHeight() : 0;
            if (scrollAmount > 0) {
                txtLogOutput.scrollTo(0, scrollAmount);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Riivolution Mod Support (XML Inspect, Options Dialog, Cooking & Manager)
    // -------------------------------------------------------------------------
    private void openModSourceChooser() {
        if (mIsCooking) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
            appendLog("[Permission] All Files Access is required to read and install Riivolution mods.");
            Toast.makeText(this, "Please allow 'All files access' first.", Toast.LENGTH_LONG).show();
            checkStoragePermissions();
            return;
        }
        String[] items = {
            "Select Riivolution Mod Archive (.zip)",
            "Select Riivolution Mod Folder (riivolution/*.xml)",
            "Scan /sdcard/Riivolution & Downloads"
        };
        new AlertDialog.Builder(this)
            .setTitle("Install Riivolution Mod")
            .setItems(items, (dialog, which) -> {
                if (which == 0) {
                    Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
                    intent.setType("*/*");
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    startActivityForResult(Intent.createChooser(intent, "Select Riivolution Mod (.zip)"), REQ_PICK_MOD_FILE);
                } else if (which == 1) {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, REQ_PICK_MOD_FOLDER);
                } else {
                    scanForRiivolutionFolders();
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void scanForRiivolutionFolders() {
        List<File> candidates = new ArrayList<>();
        File ext = Environment.getExternalStorageDirectory();
        File[] roots = {
            new File(ext, "Riivolution"),
            new File(ext, "GalaxyData/riivolution"),
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
        };
        for (File r : roots) {
            if (r == null || !r.exists()) continue;
            if (new File(r, "riivolution").isDirectory()) {
                candidates.add(r);
            }
            File[] children = r.listFiles();
            if (children == null) continue;
            for (File ch : children) {
                String low = ch.getName().toLowerCase();
                if (ch.isDirectory() && new File(ch, "riivolution").isDirectory()) {
                    candidates.add(ch);
                } else if (ch.isFile() && low.endsWith(".zip") &&
                           (low.contains("riiv") || low.contains("mod") || low.contains("neo") || low.contains("galaxy"))) {
                    candidates.add(ch);
                }
            }
        }
        if (candidates.isEmpty()) {
            Toast.makeText(this, "No Riivolution packs found in /sdcard/Riivolution or Downloads.", Toast.LENGTH_LONG).show();
            appendLog("[Riivolution] No auto-detected packs found. Pick a .zip or folder directly.");
            return;
        }
        String[] labels = new String[candidates.size()];
        for (int i = 0; i < candidates.size(); i++) {
            labels[i] = candidates.get(i).getName() + " (" + candidates.get(i).getParentFile().getName() + ")";
        }
        new AlertDialog.Builder(this)
            .setTitle("Detected Riivolution Mods")
            .setItems(labels, (d, idx) -> beginRiivolutionInspectAndInstall(candidates.get(idx).getAbsolutePath()))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void setCookingUiBusy(boolean busy, String statusMsg) {
        mIsCooking = busy;
        if (btnStartCooking != null) btnStartCooking.setEnabled(!busy);
        if (btnSelectDisc != null) btnSelectDisc.setEnabled(!busy);
        if (btnSelectFolder != null) btnSelectFolder.setEnabled(!busy);
        if (btnAutoDetect != null) btnAutoDetect.setEnabled(!busy);
        if (btnInstallMod != null) btnInstallMod.setEnabled(!busy);
        if (btnManageMods != null) btnManageMods.setEnabled(!busy);
        if (progressBar != null) {
            progressBar.setVisibility(busy ? View.VISIBLE : View.GONE);
            if (busy) progressBar.setIndeterminate(true);
        }
        if (statusMsg != null && txtProgressStatus != null) {
            txtProgressStatus.setText(statusMsg);
        }
    }

    private void beginRiivolutionInspectAndInstall(final String modSourcePath) {
        if (mIsCooking) return;
        setCookingUiBusy(true, "Inspecting Riivolution pack...");
        appendLog("==============================================");
        appendLog("[Riivolution] Inspecting: " + modSourcePath);

        new Thread(() -> {
            File modTempDir = null;
            File uriImportDir = new File(getFilesDir(), "mod_uri_import");
            try {
                CookerEngine.prepareAssets(InstallerActivity.this, new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int percent, String message) {}
                    @Override public void onFinished(boolean success, String message) {}
                });

                String pythonHome = new File(getFilesDir(), "pythonhome").getAbsolutePath();
                String scriptsDir = new File(getFilesDir(), "scripts").getAbsolutePath();

                File sourceFile = CookerEngine.resolveOrImportSource(
                    InstallerActivity.this, modSourcePath, uriImportDir,
                    new CookerEngine.CookerCallback() {
                        @Override public void onLog(String line) { appendLog(line); }
                        @Override public void onProgress(int percent, String message) {
                            mHandler.post(() -> {
                                if (message != null) txtProgressStatus.setText(message);
                            });
                        }
                        @Override public void onFinished(boolean success, String message) {}
                    }
                );

                String inspectPath = sourceFile.getAbsolutePath();
                boolean unpackedFromZip = false;
                if (sourceFile.isFile() && sourceFile.getName().toLowerCase().endsWith(".zip")) {
                    modTempDir = new File(getFilesDir(), "riiv_zip_cache");
                    CookerEngine.deleteRecursive(modTempDir);
                    modTempDir.mkdirs();
                    boolean ok = CookerEngine.extractZip(sourceFile, modTempDir, new CookerEngine.CookerCallback() {
                        @Override public void onLog(String line) { appendLog(line); }
                        @Override public void onProgress(int percent, String message) {
                            mHandler.post(() -> {
                                if (message != null) txtProgressStatus.setText(message);
                            });
                        }
                        @Override public void onFinished(boolean success, String message) {}
                    });
                    if (!ok) {
                        throw new RuntimeException("Failed to unpack Riivolution ZIP archive.");
                    }
                    inspectPath = modTempDir.getAbsolutePath();
                    unpackedFromZip = true;
                }

                String jsonStr = CookerEngine.inspectRiivolution(pythonHome, scriptsDir, inspectPath);
                JSONObject root = new JSONObject(jsonStr != null ? jsonStr : "{\"packs\":[]}");
                JSONArray packs = root.optJSONArray("packs");
                if (packs == null || packs.length() == 0) {
                    if (unpackedFromZip) CookerEngine.deleteRecursive(modTempDir);
                    CookerEngine.deleteRecursive(uriImportDir);
                    throw new RuntimeException("No riivolution/*.xml or valid SMG mod assets found inside selected source.");
                }

                final String finalInspectPath = inspectPath;
                final File finalTempDir = unpackedFromZip ? modTempDir : null;
                final File finalUriDir = uriImportDir;

                boolean needsOptionsDialog = packs.length() > 1;
                if (!needsOptionsDialog) {
                    JSONObject p0 = packs.getJSONObject(0);
                    JSONArray opts = p0.optJSONArray("options");
                    needsOptionsDialog = (opts != null && opts.length() > 0) || p0.optInt("total_memory_patches", 0) > 0;
                }

                if (needsOptionsDialog) {
                    mHandler.post(() -> {
                        setCookingUiBusy(false, "Configure Riivolution options...");
                        showRiivolutionOptionsDialog(packs, finalInspectPath, finalTempDir, finalUriDir);
                    });
                } else {
                    String packId = packs.getJSONObject(0).optString("id", "");
                    cookRiivolutionModOnWorker(finalInspectPath, packId, "{}", finalTempDir, finalUriDir);
                }
            } catch (Exception e) {
                Log.e(TAG, "Riivolution inspection error", e);
                appendLog("[ERROR] " + e.getMessage());
                mHandler.post(() -> {
                    setCookingUiBusy(false, "Riivolution inspection failed.");
                    Toast.makeText(InstallerActivity.this, e.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        }).start();
    }

    private void showRiivolutionOptionsDialog(
            final JSONArray packs, final String sourceDir,
            final File tempZipDir, final File uriImportDir) {
        try {
            final int[] selectedPackIdx = {0};
            for (int i = 0; i < packs.length(); i++) {
                if (packs.getJSONObject(i).optBoolean("game_compatible", true)) {
                    selectedPackIdx[0] = i;
                    break;
                }
            }

            ScrollView scroll = new ScrollView(this);
            LinearLayout container = new LinearLayout(this);
            container.setOrientation(LinearLayout.VERTICAL);
            int pad = (int) (16 * getResources().getDisplayMetrics().density);
            container.setPadding(pad, pad, pad, pad);
            scroll.addView(container);

            final Spinner packSpinner;
            if (packs.length() > 1) {
                TextView lbl = new TextView(this);
                lbl.setText("Mod Pack XML:");
                lbl.setTextSize(14);
                container.addView(lbl);

                List<String> packNames = new ArrayList<>();
                for (int i = 0; i < packs.length(); i++) {
                    JSONObject p = packs.getJSONObject(i);
                    String compat = p.optBoolean("game_compatible", true) ? "" : " [Non-SMG1]";
                    packNames.add(p.optString("name", p.optString("id")) + compat);
                }
                packSpinner = new Spinner(this);
                packSpinner.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, packNames));
                packSpinner.setSelection(selectedPackIdx[0]);
                container.addView(packSpinner);
            } else {
                packSpinner = null;
            }

            final LinearLayout optionsHolder = new LinearLayout(this);
            optionsHolder.setOrientation(LinearLayout.VERTICAL);
            container.addView(optionsHolder);

            final List<Map.Entry<String, Spinner>> spinners = new ArrayList<>();
            final Runnable rebuildOptions = () -> {
                optionsHolder.removeAllViews();
                spinners.clear();
                try {
                    JSONObject pack = packs.getJSONObject(selectedPackIdx[0]);
                    int memCount = pack.optInt("total_memory_patches", 0);
                    if (memCount > 0) {
                        TextView warn = new TextView(this);
                        warn.setText("Note: " + memCount + " Wii PowerPC <memory> patches detected. "
                            + "Asset mods will be cooked and enabled; Wii PPC assembly patches are skipped on ARM64.");
                        warn.setPadding(0, 8, 0, 12);
                        warn.setTextColor(0xFFF6AD55);
                        warn.setTextSize(12);
                        optionsHolder.addView(warn);
                    }
                    JSONArray opts = pack.optJSONArray("options");
                    if (opts != null) {
                        for (int i = 0; i < opts.length(); i++) {
                            JSONObject opt = opts.getJSONObject(i);
                            TextView optLbl = new TextView(this);
                            optLbl.setText(opt.optString("section", "") + " — " + opt.optString("name", opt.optString("id")));
                            optLbl.setPadding(0, 10, 0, 4);
                            optLbl.setTextSize(13);
                            optionsHolder.addView(optLbl);

                            JSONArray choices = opt.optJSONArray("choices");
                            List<String> chNames = new ArrayList<>();
                            if (choices != null) {
                                for (int c = 0; c < choices.length(); c++) {
                                    chNames.add(choices.getJSONObject(c).optString("name", "Choice " + c));
                                }
                            }
                            Spinner sp = new Spinner(this);
                            sp.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, chNames));
                            int defVal = opt.optInt("default", 0);
                            if (defVal >= 0 && defVal < chNames.size()) sp.setSelection(defVal);
                            optionsHolder.addView(sp);
                            spinners.add(new LinkedHashMap.SimpleEntry<>(opt.optString("key"), sp));
                        }
                    }
                } catch (Exception ignored) {}
            };

            rebuildOptions.run();
            if (packSpinner != null) {
                packSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(android.widget.AdapterView<?> parent, View view, int pos, long id) {
                        selectedPackIdx[0] = pos;
                        rebuildOptions.run();
                    }
                    @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
                });
            }

            String title = packs.length() == 1
                ? "Riivolution: " + packs.getJSONObject(0).optString("name", "Mod Options")
                : "Configure Riivolution Mod";

            new AlertDialog.Builder(this)
                .setTitle(title)
                .setView(scroll)
                .setPositiveButton("Cook & Install Mod", (d, w) -> {
                    try {
                        JSONObject pack = packs.getJSONObject(selectedPackIdx[0]);
                        String packId = pack.optString("id", "");
                        JSONObject optChoices = new JSONObject();
                        for (Map.Entry<String, Spinner> entry : spinners) {
                            optChoices.put(entry.getKey(), entry.getValue().getSelectedItemPosition());
                        }
                        JSONObject wrapper = new JSONObject();
                        wrapper.put(packId, optChoices);
                        setCookingUiBusy(true, "Cooking Riivolution mod assets...");
                        new Thread(() -> cookRiivolutionModOnWorker(
                            sourceDir, packId, wrapper.toString(), tempZipDir, uriImportDir)).start();
                    } catch (Exception ex) {
                        Toast.makeText(this, "Error starting mod cook: " + ex.getMessage(), Toast.LENGTH_LONG).show();
                    }
                })
                .setNegativeButton("Cancel", (d, w) -> {
                    if (tempZipDir != null) CookerEngine.deleteRecursive(tempZipDir);
                    if (uriImportDir != null) CookerEngine.deleteRecursive(uriImportDir);
                })
                .show();
        } catch (Exception e) {
            appendLog("[ERROR] Option UI failed: " + e.getMessage());
        }
    }

    private void cookRiivolutionModOnWorker(
            final String sourceDir, final String packId, final String optionsJson,
            final File tempZipDir, final File uriImportDir) {
        try {
            String pythonHome = new File(getFilesDir(), "pythonhome").getAbsolutePath();
            String scriptsDir = new File(getFilesDir(), "scripts").getAbsolutePath();
            File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
            if (!galaxyDataDir.exists()) galaxyDataDir.mkdirs();

            int ret = CookerEngine.runRiivolutionCooker(
                pythonHome, scriptsDir, sourceDir, galaxyDataDir.getAbsolutePath(),
                packId != null ? packId : "", optionsJson != null ? optionsJson : "{}",
                new CookerEngine.CookerCallback() {
                    @Override public void onLog(String line) { appendLog(line); }
                    @Override public void onProgress(int percent, String message) {
                        mHandler.post(() -> {
                            if (percent >= 0) {
                                progressBar.setIndeterminate(false);
                                progressBar.setProgress(percent);
                            }
                            if (message != null) txtProgressStatus.setText(message);
                        });
                    }
                    @Override public void onFinished(boolean success, String message) {}
                }
            );

            if (tempZipDir != null && tempZipDir.exists()) CookerEngine.deleteRecursive(tempZipDir);
            if (uriImportDir != null && uriImportDir.exists()) CookerEngine.deleteRecursive(uriImportDir);

            mHandler.post(() -> {
                setCookingUiBusy(false, ret == 0 ? "Riivolution mod installed & enabled!" : "Mod cooking failed.");
                if (ret == 0) {
                    if (btnLaunchGame != null) {
                        btnLaunchGame.setVisibility(View.VISIBLE);
                        btnLaunchGame.setText("Return to Game 🚀");
                    }
                    Toast.makeText(InstallerActivity.this, "Riivolution mod installed! Restart game if already running.", Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(InstallerActivity.this, "Riivolution cooking failed (code " + ret + ")", Toast.LENGTH_LONG).show();
                }
                flushLog();
            });
        } catch (Exception e) {
            appendLog("[ERROR] " + e.getMessage());
            mHandler.post(() -> {
                setCookingUiBusy(false, "Mod cooking error.");
                flushLog();
            });
        }
    }

    private void showManageModsDialog() {
        File galaxyDataDir = new File(Environment.getExternalStorageDirectory(), "GalaxyData");
        File modsDir = new File(galaxyDataDir, "mods");
        File cfgFile = new File(modsDir, "mods_config.ini");

        final List<String> modIds = new ArrayList<>();
        final List<Boolean> modEnabled = new ArrayList<>();
        final List<String> labels = new ArrayList<>();

        if (cfgFile.isFile()) {
            try (BufferedReader br = new BufferedReader(new FileReader(cfgFile))) {
                String line;
                while ((line = br.readLine()) != null) {
                    String s = line.trim();
                    if (s.isEmpty() || s.startsWith("#")) continue;
                    int bar = s.indexOf('|');
                    if (bar <= 0) continue;
                    boolean en = "enabled".equalsIgnoreCase(s.substring(0, bar).trim());
                    String mid = s.substring(bar + 1).trim();
                    File modFolder = new File(modsDir, mid);
                    if (!modFolder.isDirectory()) continue;

                    String displayName = mid;
                    String fileCount = "";
                    String ppcCount = "";
                    File iniFile = new File(modFolder, "mod.ini");
                    if (iniFile.isFile()) {
                        try (BufferedReader mbr = new BufferedReader(new FileReader(iniFile))) {
                            String ml;
                            while ((ml = mbr.readLine()) != null) {
                                int eq = ml.indexOf('=');
                                if (eq <= 0) continue;
                                String k = ml.substring(0, eq).trim();
                                String v = ml.substring(eq + 1).trim();
                                if ("name".equals(k) && !v.isEmpty()) displayName = v;
                                else if ("file_count".equals(k)) fileCount = v;
                                else if ("ppc_patches".equals(k)) ppcCount = v;
                            }
                        } catch (Exception ignored) {}
                    }
                    String sub = displayName + (fileCount.isEmpty() ? "" : " (" + fileCount + " files)");
                    if (!ppcCount.isEmpty() && !"0".equals(ppcCount)) {
                        sub += " [+" + ppcCount + " PPC skipped]";
                    }
                    modIds.add(mid);
                    modEnabled.add(en);
                    labels.add(sub);
                }
            } catch (Exception ignored) {}
        }

        if (modIds.isEmpty()) {
            new AlertDialog.Builder(this)
                .setTitle("Installed Riivolution Mods")
                .setMessage("No Riivolution mods are installed yet.\n\nTap 'Install Riivolution Mod (.zip / Folder)' to install a mod pack into /sdcard/GalaxyData/mods/.")
                .setPositiveButton("Install Mod", (d, w) -> openModSourceChooser())
                .setNegativeButton("Close", null)
                .show();
            return;
        }

        final boolean[] checked = new boolean[modIds.size()];
        for (int i = 0; i < modIds.size(); i++) checked[i] = modEnabled.get(i);

        new AlertDialog.Builder(this)
            .setTitle("Installed Riivolution Mods (Priority Order)")
            .setMultiChoiceItems(labels.toArray(new String[0]), checked, (d, which, isChecked) -> checked[which] = isChecked)
            .setPositiveButton("Save Mod State", (d, w) -> {
                try {
                    if (!modsDir.exists()) modsDir.mkdirs();
                    try (FileWriter fw = new FileWriter(cfgFile)) {
                        fw.write("# GalaxyMobile Riivolution Mods Config\n");
                        fw.write("# Order from top to bottom = highest to lowest priority\n");
                        for (int i = 0; i < modIds.size(); i++) {
                            fw.write((checked[i] ? "enabled|" : "disabled|") + modIds.get(i) + "\n");
                        }
                    }
                    appendLog("[Riivolution] Saved mods_config.ini (" + modIds.size() + " mod(s)).");
                    Toast.makeText(this, "Saved! Restart game to apply mod changes.", Toast.LENGTH_LONG).show();
                } catch (Exception e) {
                    Toast.makeText(this, "Failed saving mod config: " + e.getMessage(), Toast.LENGTH_LONG).show();
                }
            })
            .setNeutralButton("Delete Disabled", (d, w) -> {
                int removed = 0;
                try {
                    try (FileWriter fw = new FileWriter(cfgFile)) {
                        fw.write("# GalaxyMobile Riivolution Mods Config\n");
                        fw.write("# Order from top to bottom = highest to lowest priority\n");
                        for (int i = 0; i < modIds.size(); i++) {
                            if (checked[i]) {
                                fw.write("enabled|" + modIds.get(i) + "\n");
                            } else {
                                CookerEngine.deleteRecursive(new File(modsDir, modIds.get(i)));
                                removed++;
                            }
                        }
                    }
                    Toast.makeText(this, "Removed " + removed + " unchecked mod(s).", Toast.LENGTH_SHORT).show();
                } catch (Exception ignored) {}
            })
            .setNegativeButton("Cancel", null)
            .show();
    }
}
