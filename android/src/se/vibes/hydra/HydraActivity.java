package se.vibes.hydra;

import android.content.Intent;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import org.qtproject.qt.android.HydraSurfaceFeed;
import org.qtproject.qt.android.bindings.QtActivity;

/**
 * Qt's activity, with one override: remember the intent that just arrived.
 *
 * **Why this class exists at all.** The activity is `singleTop`, so a second
 * `VIEW` request while the browser is already running is delivered as
 * `onNewIntent` rather than as a fresh activity -- and Qt's implementation
 * does not call `setIntent`, so `getIntent()` goes on answering with the
 * intent the task was *started* with. The C++ side polls that on activation
 * (see `main_window::open_handed_url`), and it polled a stale one: measured,
 * a cold start opened the page it was handed and a running browser ignored
 * it completely. That is the common case rather than the rare one, since
 * handing a link to a browser you already have open is what people do.
 *
 * The alternative was `QtAndroidPrivate::registerNewIntentListener`, which is
 * private Qt API this project would then be pinned to across kit upgrades --
 * a coupling `android/build.gradle` already carries once and did not want a
 * second time. Four lines of Java need nothing that can move.
 *
 * `super` is still called, so anything Qt does with a new intent still
 * happens; the only change is that the activity now remembers it.
 */
public class HydraActivity extends QtActivity {
    /*
     * **Qt's main thread can wait for ever for a surface while the activity is
     * stopped** (project.md, "Qt's main thread can wedge in eglSurface()"):
     * stopping takes the surface away, a moment's resume queued behind it makes
     * Qt paint, and the paint waits with no timeout for a surface a stopped
     * activity never gets. Downloads, torrents and anything else on that thread
     * stop until hydra is shown again. Reproduced here on the emulator by
     * tool/check-surface-wait.sh. beerssh found it and fixed it this way first.
     *
     * So the main thread beats once a second (main.cpp), and while the activity
     * is stopped a thread of this class's own watches the beat. Silent for four
     * seconds, Qt's windows are handed a surface nobody sees, which ends the
     * wait; starting again brings the real one.
     */
    private static volatile long lastBeat = SystemClock.uptimeMillis();
    private static volatile HydraActivity stopped;
    private static Handler watch;
    private static ImageReader spare;

    /** From Qt's main thread, once a second, through JNI. */
    public static void beat() {
        lastBeat = SystemClock.uptimeMillis();
    }

    private static final Runnable CHECK = new Runnable() {
        @Override public void run() {
            final HydraActivity activity = stopped;
            if (activity == null)
                return;
            if (SystemClock.uptimeMillis() - lastBeat > 4000)
                activity.runOnUiThread(() -> release(activity));
            watch.postDelayed(this, 2000);
        }
    };

    private static void release(HydraActivity activity) {
        if (stopped != activity)
            return;
        if (spare == null) {
            spare = ImageReader.newInstance(64, 64, PixelFormat.RGBA_8888, 2);
            spare.setOnImageAvailableListener(reader -> {
                final Image image = reader.acquireLatestImage();
                if (image != null)
                    image.close();
            }, new Handler(Looper.getMainLooper()));
        }
        final int windows = HydraSurfaceFeed.feed(activity.getWindow().getDecorView(),
                                                  spare.getSurface());
        Log.w("hydra", "main loop silent while stopped: spare surface to "
                       + windows + " window(s)");
    }

    @Override
    protected void onStart() {
        stopped = null;
        super.onStart();
    }

    @Override
    protected void onStop() {
        super.onStop();
        if (watch == null) {
            final HandlerThread thread = new HandlerThread("hydra surface watch");
            thread.start();
            watch = new Handler(thread.getLooper());
        }
        lastBeat = SystemClock.uptimeMillis();
        stopped = this;
        watch.removeCallbacks(CHECK);
        watch.postDelayed(CHECK, 2000);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        setIntent(intent);
        super.onNewIntent(intent);
    }
}
