package com.galaxy.installer;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

public class GalaxyFileProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    private File resolveFile(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null) return null;
        if (getContext() != null) {
            File f = new File(getContext().getExternalCacheDir(), name);
            if (f.exists()) return f;
            f = new File(getContext().getCacheDir(), name);
            if (f.exists()) return f;
        }
        File dl = new File("/sdcard/Download", name);
        if (dl.exists()) return dl;
        return null;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File file = resolveFile(uri);
        if (file != null && file.exists()) {
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        throw new FileNotFoundException("Cannot resolve: " + uri);
    }

    @Override
    public String getType(Uri uri) {
        return "application/vnd.android.package-archive";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File file = resolveFile(uri);
        if (file == null) return null;
        if (projection == null) {
            projection = new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        }
        MatrixCursor cursor = new MatrixCursor(projection, 1);
        MatrixCursor.RowBuilder row = cursor.newRow();
        for (String col : projection) {
            if (OpenableColumns.DISPLAY_NAME.equals(col)) {
                row.add(file.getName());
            } else if (OpenableColumns.SIZE.equals(col)) {
                row.add(file.length());
            } else {
                row.add(null);
            }
        }
        return cursor;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { return 0; }
}
