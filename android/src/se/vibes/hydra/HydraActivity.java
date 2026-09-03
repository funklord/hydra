package se.vibes.hydra;

import android.content.Intent;

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
    @Override
    protected void onNewIntent(Intent intent) {
        setIntent(intent);
        super.onNewIntent(intent);
    }
}
