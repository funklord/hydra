package se.vibes.hydra;

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

    /**
     * ACTION_VIEW with the address and **no type at all**, which is the
     * opposite decision from openMedia above and is deliberate.
     *
     * openMedia is handing over a stream somebody has already found, so naming
     * a media type keeps browsers out of the chooser. This is handing over a
     * *page* — a youtube.com/watch address, say — and the apps worth reaching
     * are exactly the ones that register for that host: YouTube itself, VLC,
     * NewPipe. Every one of them resolves the page on its own, and forcing
     * video/* would hide all three behind players expecting a media file.
     *
     * The use this exists for: background audio. A page cannot keep playing
     * with the screen off -- the site pauses itself when hidden, and Android
     * suspends a process with no foreground service -- while those apps run a
     * media notification and are built for exactly that. So the browser stops
     * trying to be a media player and hands over instead.
     */
    public static boolean openExternally(final Activity a, final String url) {
        if (a == null || url == null || url.isEmpty())
            return false;
        Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            a.startActivity(i);
            return true;
        } catch (ActivityNotFoundException e) {
            return false;
        }
    }

    /**
     * The address this activity was asked to open, once.
     *
     * **Taken rather than read**, and that is the whole of the design. The
     * launch intent stays attached to the activity for as long as the task
     * lives, so a caller that merely reads it gets the same url again on
     * every resume -- and a browser that re-opens the page you arrived on
     * every time you come back to it is worse than one that cannot be handed
     * a page at all. Clearing the data as it is handed over makes the read
     * destructive, which is what "once" requires.
     *
     * Returns "" rather than null when there is nothing, so the C++ side has
     * one empty case to test instead of two.
     *
     * `singleTop` in the manifest means a second VIEW while this is already
     * running arrives as onNewIntent rather than as a fresh activity, and Qt
     * sets it as the current intent, so polling this on activation covers
     * both routes without needing an intent listener.
     */
    public static String takeViewUrl(final Activity a) {
        if (a == null)
            return "";
        Intent i = a.getIntent();
        if (i == null || !Intent.ACTION_VIEW.equals(i.getAction()))
            return "";
        Uri u = i.getData();
        if (u == null)
            return "";
        i.setData(null);
        a.setIntent(i);
        return u.toString();
    }
}
