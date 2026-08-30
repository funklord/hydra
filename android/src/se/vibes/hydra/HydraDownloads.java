package se.vibes.hydra;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Copying a finished download into the shared Downloads collection (§19).
 *
 * Qt downloads into app-private external storage, which needs no permission and
 * is invisible to everything else on the device. This puts a copy where a file
 * manager will show it, which also needs no permission — an app may always
 * insert its own MediaStore entries.
 */
public class HydraDownloads {

    /**
     * Returns the content: url of the published copy, or null.
     *
     * IS_PENDING is the part worth knowing about: an entry is created with it
     * set, so no other app sees a half-written file, and cleared when the bytes
     * are all there. Without it a file manager can list and open a download
     * that is still being copied.
     */
    public static String publish(final Activity a, final String path, final String mime) {
        if (a == null || path == null)
            return null;
        // MediaStore.Downloads arrived in API 29. Below that the shared folder
        // needs WRITE_EXTERNAL_STORAGE, which is a permission prompt for every
        // user of every version in exchange for helping the oldest ones -- so
        // this does nothing there and the C++ side says the file stayed put.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q)
            return null;

        File src = new File(path);
        if (!src.exists())
            return null;

        ContentResolver cr = a.getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, src.getName());
        if (mime != null && !mime.isEmpty() && !mime.equals("video/*"))
            values.put(MediaStore.Downloads.MIME_TYPE, mime);
        values.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
        values.put(MediaStore.Downloads.IS_PENDING, 1);

        Uri item = null;
        try {
            item = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
            if (item == null)
                return null;
            InputStream in = new FileInputStream(src);
            OutputStream out = cr.openOutputStream(item);
            try {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0)
                    out.write(buf, 0, n);
            } finally {
                in.close();
                if (out != null)
                    out.close();
            }
            values.clear();
            values.put(MediaStore.Downloads.IS_PENDING, 0);
            cr.update(item, values, null, null);
            return item.toString();
        } catch (Exception e) {
            // A pending entry left behind would be a file that never finishes
            // appearing, so take it back out rather than leaving a ghost.
            if (item != null) {
                try { cr.delete(item, null, null); } catch (Exception ignored) {}
            }
            return null;
        }
    }
}
