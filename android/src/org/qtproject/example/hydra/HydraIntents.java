// SPDX-License-Identifier: GPL-3.0-or-later
package org.qtproject.example.hydra;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;

/**
 * Handing a stream to another app (architecture doc §19).
 *
 * The desktop starts mpv or vlc as a process. Here the equivalent is an intent,
 * and which app takes it is the system's business — so this asks rather than
 * assumes, and reports back when nothing answers.
 */
public class HydraIntents {

    /**
     * ACTION_VIEW with a media type.
     *
     * The type matters more than it looks: with none, the chooser offers every
     * app that claims http, which on most devices means a browser — and handing
     * the stream to a browser is a loop back to where it came from.
     *
     * Returns false when the device has nothing to open it with, which is a real
     * case rather than a defensive one: a bare emulator image has no video app,
     * and startActivity would throw where the C++ side expects an answer.
     */
    public static boolean openMedia(final Activity a, final String url, final String mime) {
        if (a == null || url == null || url.isEmpty())
            return false;
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(Uri.parse(url), (mime == null || mime.isEmpty()) ? "video/*" : mime);
        // The player is a separate task: it outlives the browser the same way a
        // detached process does on the desktop, so backing out of it returns to
        // wherever the user was rather than into our tab.
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        // Read access travels with the intent, which matters for content: urls —
        // a stream picked from storage would otherwise arrive unreadable.
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            a.startActivity(i);
            return true;
        } catch (ActivityNotFoundException e) {
            return false;
        }
    }
}
