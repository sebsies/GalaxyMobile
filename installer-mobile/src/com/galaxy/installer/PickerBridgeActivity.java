package com.galaxy.installer;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.Toast;

import java.io.File;

/**
 * Invisible translucent bridge activity that solely handles Android SAF
 * file/folder picking or All-Files storage permission requests and forwards the
 * result straight to InstallerService so all user-facing UI stays inside the
 * in-app Comet Observatory Settings overlay.
 */
public class PickerBridgeActivity extends Activity {
    public static final String EXTRA_ACTION = "picker_action";
    public static final String ACT_PICK_DISC_FILE = "pick_disc_file";
    public static final String ACT_PICK_DISC_FOLDER = "pick_disc_folder";
    public static final String ACT_PICK_MOD_FILE = "pick_mod_file";
    public static final String ACT_PICK_MOD_FOLDER = "pick_mod_folder";
    public static final String ACT_GRANT_STORAGE = "grant_storage";

    private static final int REQ_FILE = 3001;
    private static final int REQ_FOLDER = 3002;
    private static final int REQ_STORAGE = 3003;

    private String mAction = ACT_PICK_DISC_FILE;
    private boolean mDispatched = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        overridePendingTransition(0, 0);

        Intent intent = getIntent();
        if (intent != null && intent.getStringExtra(EXTRA_ACTION) != null) {
            mAction = intent.getStringExtra(EXTRA_ACTION);
        }
        if (savedInstanceState != null) {
            mDispatched = savedInstanceState.getBoolean("dispatched", false);
        }
        if (mDispatched) return;
        mDispatched = true;

        try {
            if (ACT_GRANT_STORAGE.equals(mAction)) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    Intent pIntent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                    pIntent.setData(Uri.parse("package:" + getPackageName()));
                    try {
                        startActivityForResult(pIntent, REQ_STORAGE);
                    } catch (Exception e) {
                        startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQ_STORAGE);
                    }
                    return;
                }
                finishCleanly();
            } else if (ACT_PICK_DISC_FOLDER.equals(mAction) || ACT_PICK_MOD_FOLDER.equals(mAction)) {
                Intent tree = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                tree.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(tree, REQ_FOLDER);
            } else {
                Intent pick = new Intent(Intent.ACTION_GET_CONTENT);
                pick.setType("*/*");
                pick.addCategory(Intent.CATEGORY_OPENABLE);
                String title = ACT_PICK_MOD_FILE.equals(mAction)
                    ? "Select Riivolution Mod (.zip)"
                    : "Select Super Mario Galaxy (.rvz / .wbfs / .iso / .zip)";
                startActivityForResult(Intent.createChooser(pick, title), REQ_FILE);
            }
        } catch (Exception e) {
            Toast.makeText(this, "Picker unavailable: " + e.getMessage(), Toast.LENGTH_SHORT).show();
            finishCleanly();
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putBoolean("dispatched", mDispatched);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_STORAGE) {
            Intent svc = new Intent(this, InstallerService.class);
            svc.putExtra(InstallerService.EXTRA_ACTION, InstallerService.ACTION_SCAN);
            startService(svc);
            finishCleanly();
            return;
        }

        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                if ((data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
                    getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                }
            } catch (Exception ignored) {}

            String resolved = CookerEngine.resolveSourceFromUri(this, uri);
            String dispName = CookerEngine.getDisplayName(this, uri);
            String chosenPath = (resolved != null && !resolved.isEmpty()) ? resolved : uri.toString();
            String label = (dispName != null && !dispName.isEmpty()) ? dispName : new File(chosenPath).getName();

            boolean isMod = ACT_PICK_MOD_FILE.equals(mAction) || ACT_PICK_MOD_FOLDER.equals(mAction);
            Intent svc = new Intent(this, InstallerService.class);
            svc.putExtra(InstallerService.EXTRA_ACTION,
                isMod ? InstallerService.ACTION_INSPECT_MOD : InstallerService.ACTION_SELECT_SOURCE);
            svc.putExtra(InstallerService.EXTRA_MODE, isMod ? "mod" : "game");
            svc.putExtra(InstallerService.EXTRA_SOURCE_PATH, chosenPath);
            svc.putExtra(InstallerService.EXTRA_SOURCE_LABEL, label);
            startService(svc);
        }
        finishCleanly();
    }

    private void finishCleanly() {
        finish();
        overridePendingTransition(0, 0);
    }
}
