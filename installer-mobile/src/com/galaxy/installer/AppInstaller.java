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

    public static void setDisplayCutoutMode(Activity activity, boolean extendIntoCutout) {
        if (activity == null) return;
        activity.runOnUiThread(() -> {
            try {
                android.view.Window window = activity.getWindow();
                if (window == null) return;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    window.setDecorFitsSystemWindows(false);
                }
                android.view.WindowManager.LayoutParams lp = window.getAttributes();
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    if (extendIntoCutout) {
                        lp.layoutInDisplayCutoutMode = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                                ? android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                                : android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                    } else {
                        lp.layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER;
                    }
                    window.setAttributes(lp);
                }
                android.view.View decorView = window.getDecorView();
                if (decorView != null) {
                    decorView.setSystemUiVisibility(
                            android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                    );
                    decorView.requestLayout();
                }
            } catch (Exception e) {
                Log.e(TAG, "Failed to set display cutout mode", e);
            }
        });
    }

    public static void openUrl(Activity activity, String url) {
        if (activity == null || url == null) return;
        activity.runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                activity.startActivity(intent);
            } catch (Exception e) {
                Log.e(TAG, "Failed to open URL: " + url, e);
            }
        });
    }

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

    public static void shareLogFile(Activity activity) {
        if (activity == null) return;
        new Thread(() -> {
            try {
                File extFiles = activity.getExternalFilesDir(null);
                File intFiles = activity.getFilesDir();
                File extCache = activity.getExternalCacheDir();
                File cacheDir = activity.getCacheDir();

                File[] candidates = new File[] {
                    extFiles != null ? new File(extFiles, "petari_mobile_log.txt") : null,
                    intFiles != null ? new File(intFiles, "petari_mobile_log.txt") : null,
                    new File("/sdcard/GalaxyData", "petari_mobile_log.txt"),
                    new File("/sdcard/Android/data/" + activity.getPackageName() + "/files", "petari_mobile_log.txt"),
                    extFiles != null ? new File(extFiles, "petari_mobile_log.txt.prev") : null,
                    intFiles != null ? new File(intFiles, "petari_mobile_log.txt.prev") : null
                };

                File sourceLog = null;
                for (File c : candidates) {
                    if (c != null && c.exists() && c.length() > 0) {
                        sourceLog = c;
                        break;
                    }
                }

                File targetDir = extCache != null ? extCache : cacheDir;
                if (!targetDir.exists()) {
                    targetDir.mkdirs();
                }
                File targetLog = new File(targetDir, "petari_mobile_log.txt");

                if (sourceLog != null) {
                    try (InputStream in = new FileInputStream(sourceLog);
                         OutputStream out = new FileOutputStream(targetLog)) {
                        byte[] buf = new byte[65536];
                        int len;
                        while ((len = in.read(buf)) > 0) {
                            out.write(buf, 0, len);
                        }
                    }
                }

                if (!targetLog.exists() || targetLog.length() < 64) {
                    try {
                        Process process = Runtime.getRuntime().exec(new String[]{"logcat", "-d", "-v", "time"});
                        try (InputStream in = process.getInputStream();
                             OutputStream out = new FileOutputStream(targetLog, targetLog.exists())) {
                            byte[] buf = new byte[65536];
                            int len;
                            while ((len = in.read(buf)) > 0) {
                                out.write(buf, 0, len);
                            }
                        }
                    } catch (Exception e) {
                        Log.e(TAG, "Failed to run logcat fallback", e);
                    }
                }

                if (!targetLog.exists() || targetLog.length() == 0) {
                    try (FileOutputStream fos = new FileOutputStream(targetLog)) {
                        String info = "Device: " + Build.MANUFACTURER + " " + Build.MODEL + "\n"
                                    + "Android: " + Build.VERSION.RELEASE + " (SDK " + Build.VERSION.SDK_INT + ")\n"
                                    + "Package: " + activity.getPackageName() + "\n"
                                    + "No log file found on disk.\n";
                        fos.write(info.getBytes("UTF-8"));
                    }
                }

                activity.runOnUiThread(() -> {
                    try {
                        Uri logUri = Uri.parse("content://" + activity.getPackageName() + ".fileprovider/" + targetLog.getName());
                        Intent shareIntent = new Intent(Intent.ACTION_SEND);
                        shareIntent.setType("text/plain");
                        shareIntent.putExtra(Intent.EXTRA_STREAM, logUri);
                        shareIntent.putExtra(Intent.EXTRA_SUBJECT, "GalaxyMobile Debug Log (" + Build.MODEL + ")");
                        shareIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);

                        Intent chooser = Intent.createChooser(shareIntent, "Share GalaxyMobile Debug Log");
                        chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        activity.startActivity(chooser);
                    } catch (Exception e) {
                        Log.e(TAG, "Failed to share log file", e);
                        Toast.makeText(activity, "Failed to share log: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    }
                });
            } catch (Exception e) {
                Log.e(TAG, "Error preparing log file for sharing", e);
                activity.runOnUiThread(() -> 
                    Toast.makeText(activity, "Error preparing log: " + e.getMessage(), Toast.LENGTH_SHORT).show()
                );
            }
        }).start();
    }
}
