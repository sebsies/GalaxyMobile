package com.galaxy.installer;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class AppInstaller {
    private static final String TAG = "AppInstaller";
    public static final String PLUS_PACKAGE = "com.galaxy.mobile.plus";

    public static boolean isPlusInstalled(Context context) {
        if (context == null) return false;
        try {
            context.getPackageManager().getPackageInfo(PLUS_PACKAGE, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        } catch (Exception ignored) {
            return false;
        }
    }

    public static void installOrOpenPlus(Activity activity) {
        if (activity == null) return;

        if (isPlusInstalled(activity)) {
            Intent launchIntent = activity.getPackageManager().getLaunchIntentForPackage(PLUS_PACKAGE);
            if (launchIntent != null) {
                activity.startActivity(launchIntent);
                return;
            }
        }

        // Install bundled GalaxyMobilePlus.apk
        new Thread(() -> {
            try {
                // Check if unknown sources permission is granted on Android 8+
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    if (!activity.getPackageManager().canRequestPackageInstalls()) {
                        activity.runOnUiThread(() -> {
                            Toast.makeText(activity, "Please allow GalaxyMobile to install apps", Toast.LENGTH_LONG).show();
                            try {
                                Intent permIntent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES);
                                permIntent.setData(Uri.parse("package:" + activity.getPackageName()));
                                permIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                                activity.startActivity(permIntent);
                            } catch (Exception e) {
                                Intent generic = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES);
                                activity.startActivity(generic);
                            }
                        });
                        return;
                    }
                }

                activity.runOnUiThread(() -> 
                    Toast.makeText(activity, "Preparing GalaxyMobile+ installation...", Toast.LENGTH_SHORT).show()
                );

                // 1. Extract APK from assets to cache
                File cacheDir = activity.getExternalCacheDir();
                if (cacheDir == null) cacheDir = activity.getCacheDir();
                File outApk = new File(cacheDir, "GalaxyMobilePlus.apk");

                try (InputStream in = activity.getAssets().open("GalaxyMobilePlus.apk");
                     OutputStream out = new FileOutputStream(outApk)) {
                    byte[] buf = new byte[65536];
                    int len;
                    while ((len = in.read(buf)) > 0) {
                        out.write(buf, 0, len);
                    }
                }

                // 2. Also copy to Downloads folder as convenient backup
                try {
                    File dlDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                    if (dlDir != null && dlDir.exists()) {
                        File dlApk = new File(dlDir, "GalaxyMobilePlus.apk");
                        try (InputStream in = new FileInputStream(outApk);
                             OutputStream out = new FileOutputStream(dlApk)) {
                            byte[] buf = new byte[65536];
                            int len;
                            while ((len = in.read(buf)) > 0) {
                                out.write(buf, 0, len);
                            }
                        }
                    }
                } catch (Exception ignored) {}

                // 3. Launch package installer using GalaxyFileProvider
                activity.runOnUiThread(() -> {
                    try {
                        Uri apkUri = Uri.parse("content://" + activity.getPackageName() + ".fileprovider/" + outApk.getName());
                        Intent installIntent = new Intent(Intent.ACTION_VIEW);
                        installIntent.setDataAndType(apkUri, "application/vnd.android.package-archive");
                        installIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                        activity.startActivity(installIntent);
                    } catch (Exception e) {
                        Log.e(TAG, "Failed to launch package installer", e);
                        Toast.makeText(activity, "Installer error: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    }
                });

            } catch (Exception e) {
                Log.e(TAG, "Failed to extract GalaxyMobilePlus.apk", e);
                activity.runOnUiThread(() -> 
                    Toast.makeText(activity, "Failed to load GalaxyMobile+ APK: " + e.getMessage(), Toast.LENGTH_LONG).show()
                );
            }
        }).start();
    }
}
