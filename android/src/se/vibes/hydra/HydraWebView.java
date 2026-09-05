package se.vibes.hydra;

import android.annotation.TargetApi;
import android.app.Activity;
import android.os.Build;
import android.graphics.Color;
import android.net.Uri;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.GeolocationPermissions;
import android.webkit.WebStorage;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebBackForwardList;
import android.webkit.WebHistoryItem;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.ValueCallback;
import android.webkit.PermissionRequest;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebResourceError;
import android.webkit.WebChromeClient;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;
import androidx.webkit.ScriptHandler;

import java.io.ByteArrayInputStream;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/**
 * A System WebView, driven from C++.
 *
 * Qt for Widgets draws into its own surface and has no way to put a native
 * Android view inside a QWidget, so the WebView is added to the Activity's
 * content view *on top* of Qt's surface and moved to wherever the C++ side says
 * the page area is. That is the whole trick, and its cost is stated in
 * android_view.h rather than hidden: this view sits above everything Qt draws,
 * so anything Qt wants to show over the page has to hide it first.
 *
 * Every method here is called from the Qt thread and hops to the UI thread,
 * because Android's WebView may only be touched on the thread that made it.
 */
public class HydraWebView {

    private static final Map<Long, WebView> VIEWS = new HashMap<>();

    /**
     * The document-start script each view currently has registered, so the
     * previous one can be removed before the next is added.
     *
     * One per view rather than one per navigation: the handler survives loads,
     * and leaving the old one in place would stack a script per page visited
     * and answer with the origin of whichever ran first.
     */
    private static final Map<Long, ScriptHandler> START_SCRIPTS = new HashMap<>();
    /**
     * getUserMedia requests waiting on an answer.
     *
     * A PermissionRequest is answered by calling grant() or deny() on it, and
     * that need not happen inside onPermissionRequest -- which is what makes
     * this workable, because the answer needs two things that are not
     * available synchronously: the shell's own site policy, which lives on
     * the Qt thread, and the Android runtime grant, which may put a dialog in
     * front of the user.
     *
     * So the request is parked here under a token, the question goes to C++,
     * and onCaptureDecision() comes back to finish it. The token is a counter
     * rather than the view id because one view can have more than one request
     * outstanding.
     */
    /**
     * Diagnostics for the part of the capture handshake the shell cannot see.
     *
     * **Most of this now belongs in the browser, not in logcat.** `filter_signals`
     * records what each site asked for and what it was told, a report carries
     * that evidence, and the annoyed dialog shows it -- so the person looking at
     * the broken page can see the answer without a cable. That is the mechanism
     * this project is built around and it should be the first thing reached for.
     *
     * What stays here is the half that happens below the shell: the raw resource
     * list Android hands over, and whether `onPermissionRequest` fired at all. A
     * request for something this class does not name, and no request whatsoever,
     * are indistinguishable from inside the browser and are the two candidates
     * when a site insists it has no camera.
     *
     * `Log.isLoggable` rather than a constant or an environment variable: a
     * constant means rebuilding to see anything, and Qt's `HYDRA_PERM_DEBUG`
     * cannot be set for an app Android launches. One command turns it on:
     *
     *     adb shell setprop log.tag.HydraPerm DEBUG
     *
     * Off by default, because these lines carry the origin of every page that
     * asks for a camera and logcat is readable by more than whoever wanted the
     * answer.
     */
    private static final String PERM_TAG = "HydraPerm";
    private static boolean permDebug() {
        return Log.isLoggable(PERM_TAG, Log.DEBUG);
    }

    private static final Map<Long, PermissionRequest> PENDING = new HashMap<>();
    private static long NEXT_TOKEN = 1;

    /**
     * The same arrangement for geolocation, which arrives through a different
     * callback and has to be parked the same way.
     *
     * Two maps rather than one of a common supertype: a `PermissionRequest` and
     * a `GeolocationPermissions.Callback` have nothing in common but being
     * answered late, and a shared map would need a cast at every use to buy
     * nothing. The token counter is shared, so a token means one thing.
     */
    private static final Map<Long, GeolocationPermissions.Callback> GEO_PENDING =
        new HashMap<>();
    private static final Map<Long, String> GEO_ORIGIN = new HashMap<>();

    /**
     * Which geolocation token a view is currently waiting on, view id to token.
     *
     * **Needed because the cancellation callback names nothing.**
     * `onGeolocationPermissionsHidePrompt` takes no arguments -- it means
     * "stop showing whatever you are showing" -- while the two maps above are
     * keyed by the token the C++ side holds. Without this there is no way to
     * say which entries the cancellation refers to, and the honest options are
     * to drop every view's or to drop none. A WebView shows one geolocation
     * prompt at a time, so one token per view is the whole of it.
     */
    private static final Map<Long, Long> GEO_TOKEN = new HashMap<>();

    /**
     * Kept so every later call can be posted to the same UI-thread queue that
     * create() used. That ordering is the whole point: runOnUiThread is FIFO, so
     * a load posted after a create runs after it, whereas looking the view up on
     * the calling thread finds nothing because create() has not run yet.
     *
     * That was the bug. load() returned silently, the WebView sat there showing
     * its own white background over the placeholder -- which looked exactly like
     * a page that failed to render, and sent two rounds of investigation at
     * Z-order and compositing instead of at ordering.
     */
    private static Activity ACTIVITY;

    /** Called from C++ when a page load starts, so the shell can follow along. */
    public static native void onUrlChanged(long id, String url);
    /**
     * Whether Back and Forward have anywhere to go.
     *
     * Pushed rather than asked for. A WebView may only be touched on the
     * thread that made it, so a synchronous query from the Qt thread would
     * have to block on Android's UI thread -- which is the shape of deadlock
     * this app has already met once, from Qt's accessibility bridge.
     */
    public static native void onNavState(long id, boolean back, boolean forward);

    /**
     * The renderer died and a replacement view has been put in its place.
     * `crashed` separates a real fault from Android reclaiming memory.
     */
    public static native void onRenderGone(long id, boolean crashed);

    /** What the page calls itself, as `onReceivedTitle` reported it. */
    public static native void onTitleChanged(long id, String title);

    /** How far the current load has got, 0..100. */
    public static native void onLoadProgress(long id, int percent);

    /** The load ended; `ok` is false when the main frame reported an error. */
    public static native void onLoadFinished(long id, boolean ok);

    /** A page went fullscreen, or came back out of it. */
    public static native void onFullscreen(long id, boolean on);

    /**
     * Asks the shared request_filter about one request. Called on the WebView's
     * network thread, not the UI thread; the C++ side only reads the policy
     * engine, which documents itself as safe for that.
     */
    public static native boolean shouldBlock(String url, String accept, String pageUrl);

    /**
     * Whether the page at pageUrl may set or send third-party cookies.
     *
     * The same per-site policy the desktop applies, answered by the same
     * request_filter, so the two backends cannot drift apart on a setting the
     * shield offers for both.
     */
    public static native boolean allowThirdPartyCookies(String pageUrl);

    /**
     * Android has finished removing cookies. `removed` is whether any went --
     * Android offers no count, which is why the C++ side leaves its counter at
     * "nobody looked" rather than filling in a number it does not have.
     */
    public static native void onCookiesCleared(boolean removed);

    /** The method list for one bridge, as JSON. Called from the page. */
    public static native String bridgeDescribe(long id, String name);

    /** Calls one bridge method. Every argument here comes from the page. */
    public static native String bridgeCall(long id, String name, String method, String args);

    /** Everything to run at the start of a page: the shim, then the scripts. */
    public static native String injectedScripts(long id);

    /**
     * The permissions shim alone, built for a url this view has not reached
     * yet. Separate from injectedScripts because it is registered *before* the
     * navigation, when the view's own url is still the page being left.
     */
    public static native String documentStartScript(long id, String url);

    /**
     * The user agent this browser should send, derived from the one the
     * System WebView offers.
     *
     * Native rather than done here, and the reason is not tidiness: the
     * desktop already corrects its own string with a tested C++ function, and
     * a second copy of those rules written in Java would drift from it. The
     * transformation is a pure function of the string, so this is the one
     * native call in this class that needs no thread hop at all.
     */
    public static native String correctedUserAgent(String offered);

    /** The same string, rewritten to claim a desktop. See setDesktopSite. */
    public static native String desktopUserAgent(String mobile);

    /** True when the shell took a navigation instead -- magnet: and the like. */
    public static native boolean takeExternalUrl(String url);

    /**
     * Whether this view may go to `url`, asked of the shell.
     *
     * A locked tab keeps the page it is on: the shell opens a sub-tab below it
     * and browsing continues there, so the navigation must not happen in this
     * WebView. False means exactly that -- do not load it -- and the tab that
     * was asked stays where it is.
     *
     * `userGesture` separates a tap from a redirect or a script, which get
     * different answers: a page that could spawn tabs by scripting its own
     * location would turn one pinned tab into a stream of them.
     */
    public static native boolean allowNavigation(long id, String url,
                                                 boolean userGesture);

    /** A page's file input was used. The answer arrives later, via deliverFiles. */
    /**
     * Asks the shell whether this origin may capture, and answers later.
     *
     * Two questions behind one call: the site policy, which is the same
     * engine the desktop asks and which lives on the Qt thread, and the
     * Android runtime grant, which may show a dialog. Neither can be answered
     * from here and both must be, so nothing is returned -- the answer
     * arrives at onCaptureDecision() with the token this was given.
     */
    public static native void requestCapture(long id, String origin,
                                            boolean video, boolean audio,
                                            long token);

    /**
     * Asks the shell whether this origin may know where the device is.
     *
     * Same shape as requestCapture and for the same two reasons: the site
     * policy lives on the Qt thread, and the Android runtime grant may put a
     * dialog in front of somebody. The answer comes back to
     * onGeolocationDecision() with this token.
     */
    public static native void requestGeolocation(long id, String origin,
                                                 long token);

    /**
     * The answer to a requestGeolocation, from C++ on the UI thread.
     *
     * `invoke` takes the origin back, so it is parked with the callback -- the
     * WebView will not accept an answer addressed to anything else, and getting
     * it wrong is a page that waits for ever rather than an error.
     *
     * The third argument is "remember this", and it is always false. Android
     * would keep the answer in its own store, invisible to this browser and not
     * clearable from it -- a second authority over a decision the shield
     * already records, which is exactly the split the profile's persistent
     * permissions were turned off to avoid on the desktop.
     */
    public static void onGeolocationDecision(final long token, final boolean granted) {
        onUi(new Runnable() { @Override public void run() {
            // The note of what this view was waiting on goes with the answer,
            // or a later cancellation would look up a token already dealt with.
            GEO_TOKEN.values().remove(Long.valueOf(token));
            GeolocationPermissions.Callback cb = GEO_PENDING.remove(token);
            String origin = GEO_ORIGIN.remove(token);
            if (cb == null) {
                if (permDebug())
                    Log.d(PERM_TAG, "geolocation decision " + granted +
                          " for token " + token + " but nothing was waiting");
                return;
            }
            if (permDebug())
                Log.d(PERM_TAG, "geolocation token=" + token +
                      (granted ? " GRANTED" : " denied") + " for " + origin);
            cb.invoke(origin, granted, false);
        } });
    }

    /**
     * The answer to a requestCapture, from C++ on the UI thread.
     *
     * Granting names the resources the page asked for rather than everything
     * it might have: a request for audio alone must not come back holding the
     * camera because the policy happened to allow both.
     *
     * A token with nothing parked under it is not an error. The view can be
     * torn down between the question and the answer, and a page that has gone
     * away has no request left to grant.
     */
    public static void onCaptureDecision(final long token, final boolean granted) {
        onUi(new Runnable() {
            @Override public void run() {
                PermissionRequest req = PENDING.remove(token);
                if (req == null) {
                    // A token nothing is waiting on: answered twice, or the
                    // view went away between the question and the answer.
                    if (permDebug())
                        Log.d(PERM_TAG, "decision " + granted + " for token " +
                              token + " but nothing was waiting on it");
                    return;
                }
                if (permDebug())
                    Log.d(PERM_TAG, "decision token=" + token +
                          (granted ? " GRANTED" : " denied"));
                if (!granted) {
                    req.deny();
                    return;
                }
                req.grant(req.getResources());
            }
        });
    }

    public static native void chooseFile(long id, boolean multiple, String accept);

    /**
     * The chooser callback for each view, held between chooseFile and
     * deliverFiles.
     *
     * A WebView keeps exactly one of these and will not open another chooser
     * until it is answered, so dropping one does not fail a single upload -- it
     * disables every file input on every later page. deliverFiles is therefore
     * called on cancel too, with nothing in it.
     */
    private static final Map<Long, ValueCallback<Uri[]>> CHOOSERS = new HashMap<>();

    /** Called from C++ with the picked urls, newline-separated, or empty. */
    public static void deliverFiles(final long id, final String joined) {
        onUi(new Runnable() { @Override public void run() {
            ValueCallback<Uri[]> cb = CHOOSERS.remove(id);
            if (cb == null)
                return;
            if (joined == null || joined.isEmpty()) {
                cb.onReceiveValue(null);   // the documented "user cancelled"
                return;
            }
            String[] parts = joined.split("\n");
            Uri[] uris = new Uri[parts.length];
            for (int i = 0; i < parts.length; ++i)
                uris[i] = Uri.parse(parts[i]);
            cb.onReceiveValue(uris);
        } });
    }

    /**
     * What the page sees as `window.hydraNative`.
     *
     * Only methods marked @JavascriptInterface are reachable, which is the whole
     * of Android's exposure control -- the rest is bridge_invoker's problem, and
     * it is written for a hostile caller because this object is reachable from
     * every frame and every origin the WebView loads.
     *
     * These run on a binder thread, not the UI thread; the C++ side hops to the
     * Qt thread and waits.
     */
    private static class Native {
        private final long id;
        Native(long id) { this.id = id; }

        @JavascriptInterface
        public String describe(String name) { return bridgeDescribe(id, name); }

        @JavascriptInterface
        public String call(String name, String method, String args) {
            return bridgeCall(id, name, method, args);
        }

        // Not part of the page's API: nothing documents it and no page has a
        // reason to call it. It is on the same bridge because there is only
        // one, and a second interface object would be a second thing every
        // page can see for no gain.
        @JavascriptInterface
        public void mediaState(boolean playing) { notePlaying(id, playing); }
    }

    /**
     * The page each WebView is on, as last reported by onPageStarted.
     *
     * shouldInterceptRequest needs the first-party host to apply per-site rules,
     * and cannot ask the WebView for it: getUrl() is UI-thread only and this
     * runs on the network thread. So the UI thread writes it here and the
     * network thread reads it.
     *
     * **Keyed by view, which it was not.** This was one `static volatile
     * String` for the whole class while up to `k_max_live_views` WebViews
     * exist, and its own description has always said "the page each WebView is
     * on" -- per-view semantics that a single field cannot provide. Whichever
     * view navigated most recently won, and every view read its answer.
     *
     * What that costs is a filter decision made against the wrong site. The
     * first-party host becomes `request_context::site_host`, which is what
     * per-site rules key on -- and the antiadblock path sets exactly such a
     * rule automatically, without being asked. So a subresource still in
     * flight from tab A -- an ad refresh timer, a lazy image, an XHR -- can
     * arrive after tab B navigates and be judged by B's host: A's allowance
     * stops applying and B's applies instead, and the same crossing runs the
     * other way round.
     *
     * A concurrent map rather than a volatile field, because the write is on
     * the UI thread and the read is on the network thread, which is the reason
     * the single field was volatile in the first place.
     */
    /**
     * Which views have had a main-frame error since their last navigation.
     *
     * Android reports the end of a load and its failure through two separate
     * callbacks, so success is the absence of an error rather than something
     * it states. Keyed by view and cleared at `onPageStarted`, or a failure
     * would outlive the page it belonged to.
     */
    private static final Map<Long, Boolean> FAILED =
        new java.util.concurrent.ConcurrentHashMap<Long, Boolean>();

    /**
     * Which views have already reported the end of their current navigation.
     *
     * **A load ends once, and Android can say so three ways.** `onPageFinished`
     * does not reliably arrive for a main-frame failure; `onReceivedError`
     * does, but the error page Chromium then draws is *itself* a load, so
     * progress runs up again behind it. Measured on this handset: reporting
     * the failure from `onReceivedError` alone left the browser showing a
     * Stop button and a full progress bar over `ERR_NAME_NOT_RESOLVED`,
     * because the error document's own progress turned loading back on.
     *
     * So all three routes call `endLoad`, the first one wins, and
     * `onPageStarted` opens the next navigation by clearing this.
     */
    private static final Map<Long, Boolean> ENDED =
        new java.util.concurrent.ConcurrentHashMap<Long, Boolean>();

    /**
     * The view a page is showing fullscreen, and the callback that dismisses
     * it. One at a time: Android hands over a single custom view, and a
     * second request while one is up is a page misbehaving rather than a case
     * to support.
     */
    private static View FULLSCREEN_VIEW = null;
    private static WebChromeClient.CustomViewCallback FULLSCREEN_CB = null;
    private static long FULLSCREEN_ID = 0;

    /**
     * Put the page's fullscreen view up, or take it down.
     *
     * **Without `onShowCustomView` a video simply does not go fullscreen.**
     * The default implementation does nothing, so the control on the player
     * is dead -- on the platform where a fullscreen video matters most. The
     * page is told nothing either, so it sits in whatever layout it chose.
     *
     * Attached the way the WebViews are, and for the same reason: Qt draws
     * into a `SurfaceView` with its Z-order set, which punches through
     * anything merely layered on top, so being added later is not enough.
     */
    private static void showFullscreen(final long id, final View v,
                                        final WebChromeClient.CustomViewCallback cb) {
        onUi(new Runnable() {
            @Override public void run() {
                if (ACTIVITY == null || FULLSCREEN_VIEW != null) {
                    if (cb != null)
                        cb.onCustomViewHidden();
                    return;
                }
                FULLSCREEN_VIEW = v;
                FULLSCREEN_CB = cb;
                FULLSCREEN_ID = id;
                ACTIVITY.addContentView(v, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
                v.bringToFront();
                v.setZ(200f);   // above the page, which is already above Qt
                onFullscreen(id, true);
            }
        });
    }

    private static void hideFullscreen() {
        onUi(new Runnable() {
            @Override public void run() {
                View v = FULLSCREEN_VIEW;
                if (v == null)
                    return;
                final long id = FULLSCREEN_ID;
                ViewGroup p = (ViewGroup) v.getParent();
                if (p != null)
                    p.removeView(v);
                FULLSCREEN_VIEW = null;
                FULLSCREEN_ID = 0;
                // **The page is told last.** Its callback is what lets it put
                // the player back the way it was, and calling it before the
                // view is detached leaves the old surface on screen over the
                // restored layout.
                WebChromeClient.CustomViewCallback cb = FULLSCREEN_CB;
                FULLSCREEN_CB = null;
                if (cb != null)
                    cb.onCustomViewHidden();
                onFullscreen(id, false);
            }
        });
    }

    /** Leave fullscreen because the shell said so, not the page. */
    public static void exitFullscreen(long id) {
        hideFullscreen();
    }

    /** Report the end of a navigation exactly once. */
    private static void endLoad(long id, boolean ok) {
        if (ENDED.putIfAbsent(Long.valueOf(id), Boolean.TRUE) == null)
            onLoadFinished(id, ok);
    }

    private static final Map<Long, String> PAGE_URLS =
        new java.util.concurrent.ConcurrentHashMap<Long, String>();

    /** An empty 200, which is how a WebView says "blocked". */
    private static WebResourceResponse blocked() {
        return new WebResourceResponse("text/plain", "utf-8",
                                       new ByteArrayInputStream(new byte[0]));
    }

    // The Activity is handed in rather than fetched: QtNative.activity() is not
    // public API in Qt 6.11, and reaching for it compiles until the day it does
    // not. C++ already has the context and can pass it.
    public static void create(final Activity a, final long id) {
        if (a == null)
            return;
        ACTIVITY = a;
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (VIEWS.containsKey(id))
                    return;
                Player w = new Player(a);
                w.getSettings().setJavaScriptEnabled(true);
                w.getSettings().setDomStorageEnabled(true);
                // **What this browser says it is.** The WebView's own default
                // announces itself three times -- `wv`, `Version/4.0`, and the
                // device build fingerprint -- and a site working from a list of
                // known browsers reads that as something that is not one.
                // teams.microsoft.com answers "your browser isn't supported".
                //
                // Derived from what the platform offers rather than written
                // out, for the reason the desktop gives: the platform half has
                // to stay true, and a hardcoded string is wrong on the next
                // device.
                final String ua = correctedUserAgent(w.getSettings().getUserAgentString());
                if (ua != null && !ua.isEmpty())
                    w.getSettings().setUserAgentString(ua);
                // **Cookies, which nothing here had ever configured.**
                // setAcceptCookie defaults to true, so first-party cookies did
                // work; it is stated anyway, because the default is what this
                // app has just been burned by on the desktop -- the whole
                // browser was off the record for months by inheriting one.
                //
                // The third-party half is the part that was actually broken.
                // It defaults to FALSE for anything targeting Lollipop or
                // newer, so Android blocked every third-party cookie outright
                // no matter what the shield said -- and a per-site "allow"
                // that the desktop honours did nothing at all on the phone.
                // Sign-in flows that bounce through an identity provider are
                // exactly what that breaks. It is set per navigation, from the
                // policy, in onPageStarted below.
                CookieManager.getInstance().setAcceptCookie(true);
                w.setBackgroundColor(Color.WHITE);
                // **Hardware compositing, which this used to refuse.**
                //
                // It was LAYER_TYPE_SOFTWARE, and the reason was real: on the
                // *emulator*, with the default layer, the renderer process died
                // immediately, and software was the difference between a page
                // and a blank rectangle. What that bought was a WebView with no
                // GPU compositing, and the bill arrived as "YouTube has a lot of
                // flaws in its display, including the video that cannot be
                // seen" -- which is exactly the shape of a software layer:
                // transforms, effects, canvas and video all degrade together,
                // and video worst of all, because a SurfaceTexture has nowhere
                // to composite to.
                //
                // Re-measured on a Galaxy Note 9 against system WebView 151:
                // the renderer does not die. example.com renders, m.youtube.com
                // renders in full, and a video plays and is visible, with no
                // renderer death in the log through any of it. The original
                // finding was true of an emulator and an older WebView, and was
                // avoided rather than diagnosed; it is not true here.
                //
                // **The escape hatch is deliberate.** A finding that was real
                // once may be real again on hardware nobody here has, and the
                // symptom -- a blank rectangle where the page should be -- is
                // one somebody could not otherwise work around.
                // `HYDRA_ANDROID_SOFTWARE_LAYER=1` puts the old behaviour back,
                // in the same shape as `HYDRA_ANDROID_WEBVIEW=0` above it.
                final boolean force_software =
                  "1".equals(System.getenv("HYDRA_ANDROID_SOFTWARE_LAYER"));
                w.setLayerType(force_software
                                 ? android.view.View.LAYER_TYPE_SOFTWARE
                                 : android.view.View.LAYER_TYPE_NONE, null);
                // Before any load, so a page cannot start without it.
                w.addJavascriptInterface(new Native(id), "hydraNative");
                w.setWebChromeClient(new WebChromeClient() {
                    /**
                     * The page said what it is called.
                     *
                     * **Without this every tab in the tree is a hostname.**
                     * The shell sets a node's title from `title_changed` and
                     * falls back to `url().host()` when nothing arrives, so
                     * on Android the fallback was the only case: every row in
                     * the tree, and the window title, said `example.com`
                     * where the desktop says what the page calls itself.
                     *
                     * `onReceivedTitle` is the only hook a plain WebView
                     * offers for this, and it fires again when a page changes
                     * its own title, which is what the desktop backend's
                     * signal does too.
                     */
                    /**
                     * How far the current load has got, 0..100.
                     *
                     * **Without this the shell never believes a load is in
                     * flight at all.** `m_loading` is set only from this
                     * signal, so the progress bar never appeared, the Reload
                     * button never became Stop, and a page that failed said
                     * nothing -- `on_load_finished(false)` is what prints
                     * "could not be loaded", and it needs a load to have
                     * started.
                     */
                    @Override
                    public void onShowCustomView(View view, CustomViewCallback cb) {
                        showFullscreen(id, view, cb);
                    }

                    @Override
                    public void onHideCustomView() {
                        hideFullscreen();
                    }

                    @Override
                    public void onProgressChanged(WebView v, int percent) {
                        // **Silent once this navigation has ended**, and that
                        // is the whole of it. `on_load_progress` in the shell
                        // sets `m_loading` *true*, so progress arriving after
                        // a load has finished turns the Stop button and the
                        // bar back on -- and Chromium's error page is itself
                        // a load, which reports progress right after the
                        // failure that ended the real one.
                        //
                        // Measured three times over: reporting the end from
                        // `onReceivedError`, then from `onPageFinished`, then
                        // from both plus progress-100, changed nothing at all
                        // -- because each fixed the ending and none of them
                        // stopped what came after it.
                        if (ENDED.containsKey(Long.valueOf(id)))
                            return;
                        onLoadProgress(id, percent);
                        // 100 is an ending too, and the only one that arrives
                        // for every outcome.
                        if (percent >= 100)
                            endLoad(id, !FAILED.containsKey(Long.valueOf(id)));
                    }

                    @Override
                    public void onReceivedTitle(WebView v, String title) {
                        onTitleChanged(id, title == null ? "" : title);
                    }

                    /**
                     * A page asked for the camera or the microphone.
                     *
                     * **The default implementation denies, silently**, which is why this
                     * override is not optional: without it no getUserMedia on Android can
                     * succeed whatever the manifest declares or the user has granted. It is
                     * the one of the three requirements with no diagnostic at all -- the
                     * page just sees a rejected promise.
                     *
                     * Only the two capture resources are considered. Anything else -- a
                     * protected media id, say -- is denied here rather than passed on,
                     * because nothing in this browser has a policy for it, and granting
                     * what you cannot describe is how a permission model stops meaning
                     * anything.
                     */
                    @Override
                    public void onPermissionRequest(final PermissionRequest req) {
                        boolean video = false, audio = false;
                        for (String r : req.getResources()) {
                            if (PermissionRequest.RESOURCE_VIDEO_CAPTURE.equals(r))
                                video = true;
                            else if (PermissionRequest.RESOURCE_AUDIO_CAPTURE.equals(r))
                                audio = true;
                        }
                        // **The raw resource list, not just what we understood
                        // of it.** The question this exists to answer is why a
                        // site that asks for a camera does not get one, and the
                        // two candidates look identical from outside: a request
                        // for something this switch does not name, and no
                        // request at all. Printing what arrived separates them.
                        if (permDebug()) {
                            StringBuilder res = new StringBuilder();
                            for (String r : req.getResources()) {
                                if (res.length() > 0) res.append(", ");
                                res.append(r);
                            }
                            Log.d(PERM_TAG, "onPermissionRequest origin=" +
                                  (req.getOrigin() != null ? req.getOrigin().toString() : "(none)") +
                                  " resources=[" + res + "]" +
                                  " -> video=" + video + " audio=" + audio);
                        }
                        if (!video && !audio) {
                            if (permDebug())
                                Log.d(PERM_TAG, "denied: nothing in it was audio "
                                      + "or video capture");
                            req.deny();
                            return;
                        }
                        final long token = NEXT_TOKEN++;
                        PENDING.put(token, req);
                        final String origin = req.getOrigin() != null
                                ? req.getOrigin().toString() : "";
                        requestCapture(id, origin, video, audio, token);
                    }

                    /**
                     * The page stopped waiting for an answer.
                     *
                     * **Parking a request and never unparking it is a leak
                     * with a second edge.** A request lives in `PENDING` from
                     * the moment it is asked until somebody answers, and if
                     * the page navigates away while the prompt is up -- a
                     * redirect, a meta refresh, a timer -- nobody ever does.
                     * Android says so through this callback, which was not
                     * overridden, so the entry stayed for the life of the
                     * process holding a `PermissionRequest` and the frame
                     * state behind it.
                     *
                     * `onCaptureDecision` already reads correctly for the
                     * aftermath -- its null branch says "answered twice, or
                     * the view went away between the question and the answer"
                     * -- and that was describing a cleanup nothing performed.
                     * It performs it now, so a late answer to a cancelled
                     * request finds nothing and says so rather than calling
                     * `grant()` on a request the page has abandoned.
                     *
                     * Searched by value because the map is keyed by the token
                     * the C++ side holds, and Android hands back the request
                     * rather than the token. The map has one entry per prompt
                     * on screen, so the scan is over a list of about one.
                     */
                    @Override
                    public void onPermissionRequestCanceled(PermissionRequest req) {
                        for (java.util.Iterator<Map.Entry<Long, PermissionRequest>> it =
                                 PENDING.entrySet().iterator(); it.hasNext(); ) {
                            Map.Entry<Long, PermissionRequest> e = it.next();
                            if (e.getValue() == req) {
                                if (permDebug())
                                    Log.d(PERM_TAG, "request cancelled by the page,"
                                          + " dropping token " + e.getKey());
                                it.remove();
                                return;
                            }
                        }
                    }

                    /**
                     * A page asking where the device is.
                     *
                     * **Not overriding this is not neutral.** The base
                     * implementation does nothing at all -- it neither grants
                     * nor denies, so the callback is never invoked and the
                     * page's `getCurrentPosition` error handler fires with
                     * "User denied Geolocation". Measured on the handset: the
                     * shield said location was allowed, the settings screen
                     * offered no location permission to grant, and the page was
                     * refused anyway, all three being the same missing piece.
                     */
                    @Override
                    public void onGeolocationPermissionsShowPrompt(
                            String origin, GeolocationPermissions.Callback cb) {
                        if (permDebug())
                            Log.d(PERM_TAG, "onGeolocationPermissionsShowPrompt "
                                  + "origin=" + origin);
                        if (cb == null)
                            return;
                        if (origin == null || origin.isEmpty()) {
                            // Nothing to attribute it to and nothing to answer
                            // with: invoke() is addressed by origin.
                            cb.invoke(origin, false, false);
                            return;
                        }
                        final long token = NEXT_TOKEN++;
                        GEO_PENDING.put(token, cb);
                        GEO_ORIGIN.put(token, origin);
                        GEO_TOKEN.put(Long.valueOf(id), Long.valueOf(token));
                        requestGeolocation(id, origin, token);
                    }

                    /**
                     * The page stopped waiting to be located.
                     *
                     * The same leak as `onPermissionRequestCanceled` and the
                     * same cause -- a request parked until somebody answers,
                     * and a page that navigates away before anybody does. Two
                     * maps held it rather than one, so it leaked twice.
                     *
                     * The callback that ends it names nothing, which is why
                     * `GEO_TOKEN` exists: it says which token this view was
                     * waiting on. The `WebView` two methods below already
                     * knows to do this for a file chooser -- "never leave one
                     * hanging" -- and this is that care applied where it was
                     * missing.
                     */
                    @Override
                    public void onGeolocationPermissionsHidePrompt() {
                        Long tok = GEO_TOKEN.remove(Long.valueOf(id));
                        if (tok == null)
                            return;
                        GEO_PENDING.remove(tok);
                        GEO_ORIGIN.remove(tok);
                        if (permDebug())
                            Log.d(PERM_TAG, "geolocation prompt withdrawn by the"
                                  + " page, dropping token " + tok);
                    }
                    // Returning true means "I will answer, later". Returning
                    // false would let the WebView fall back to nothing at all --
                    // a plain WebView has no picker of its own.
                    @Override
                    public boolean onShowFileChooser(WebView v,
                                                     ValueCallback<Uri[]> cb,
                                                     FileChooserParams params) {
                        ValueCallback<Uri[]> old = CHOOSERS.put(id, cb);
                        if (old != null)
                            old.onReceiveValue(null);   // never leave one hanging
                        boolean many = params != null &&
                            params.getMode() == FileChooserParams.MODE_OPEN_MULTIPLE;
                        StringBuilder accept = new StringBuilder();
                        if (params != null && params.getAcceptTypes() != null)
                            for (String t : params.getAcceptTypes()) {
                                if (t == null || t.isEmpty()) continue;
                                if (accept.length() > 0) accept.append(',');
                                accept.append(t);
                            }
                        chooseFile(id, many, accept.toString());
                        return true;
                    }
                });
                w.setWebViewClient(new WebViewClient() {
                    @Override
                    public void doUpdateVisitedHistory(WebView v, String url,
                                                        boolean isReload) {
                        // The callback Android provides for exactly this: the
                        // back/forward list has changed.
                        onNavState(id, v.canGoBack(), v.canGoForward());
                    }

                    /**
                     * The renderer for this tab died.
                     *
                     * **Returning false here kills the whole application
                     * process**, which is what a `WebViewClient` that does
                     * not override this does by default from API 26 onwards.
                     * So the browser vanished -- every tab, the tree since
                     * the last debounced save, and a kiosk session with
                     * nobody watching -- because one page's renderer went
                     * away. The desktop backend shows "stopped responding,
                     * Reload to try again" and carries on; this is the same
                     * event and it took the window with it.
                     *
                     * The dead `WebView` cannot be reused: the platform is
                     * explicit that the instance is finished. So it is torn
                     * down by the same path a closed tab uses, and a fresh
                     * one is built under the same id -- because the message
                     * the shell shows says *Reload to try again*, and a
                     * reload needs something to load into. Shipping the
                     * message without the view would be a promise the
                     * browser cannot keep.
                     *
                     * The new view starts with default settings; the shell
                     * re-applies the site's policy on the next navigation,
                     * which is the reload the person was just asked for.
                     */
                    @Override
                    @TargetApi(Build.VERSION_CODES.O)
                    public boolean onRenderProcessGone(WebView v,
                                                        RenderProcessGoneDetail detail) {
                        // `didCrash` false means Android reclaimed the
                        // renderer to save memory, which is not a fault and
                        // is commoner on a phone than a real crash.
                        final boolean crashed =
                          detail != null && detail.didCrash();
                        Log.w("hydra", "render process gone for view " + id
                                        + " (crashed=" + crashed + ")");
                        destroy(id);
                        create(ACTIVITY, id);
                        onRenderGone(id, crashed);
                        return true;
                    }

                    /**
                     * The load ended. Android has no "ok" here -- it reports
                     * a failure separately through `onReceivedError` -- so
                     * success is the absence of one for this navigation,
                     * which `FAILED` records.
                     */
                    @Override
                    public void onPageFinished(WebView v, String url) {
                        endLoad(id, !FAILED.containsKey(Long.valueOf(id)));
                    }

                    /**
                     * **Main frame only.** A subresource that 404s is not a
                     * page that failed to load, and reporting it as one would
                     * put "could not be loaded" under a page the person is
                     * reading.
                     */
                    @Override
                    public void onReceivedError(WebView v, WebResourceRequest req,
                                                 WebResourceError err) {
                        if (req == null || !req.isForMainFrame())
                            return;
                        // **Reported from here, not left to
                        // `onPageFinished`.** That callback does not reliably
                        // arrive for a main-frame failure: measured on this
                        // handset against `ERR_NAME_NOT_RESOLVED`, the
                        // browser sat showing a Stop button and a progress
                        // bar indefinitely while Chromium's own error page
                        // was on screen behind them.
                        //
                        // The flag stays, so that an `onPageFinished` which
                        // *does* follow is not read as a second, successful
                        // end to the same navigation.
                        FAILED.put(Long.valueOf(id), Boolean.TRUE);
                        endLoad(id, false);
                    }

                    @Override
                    public void onPageStarted(WebView v, String url, android.graphics.Bitmap f) {
                        PAGE_URLS.put(Long.valueOf(id), url);
                        // A new navigation has not failed and has not
                        // ended.
                        FAILED.remove(Long.valueOf(id));
                        ENDED.remove(Long.valueOf(id));
                        // Per navigation, because the policy is per site. This
                        // runs after the main document request has gone out and
                        // before its subresources, which is where third-party
                        // cookies actually appear -- so it lands in time for
                        // everything the setting is about. Android offers no
                        // earlier hook that knows the url.
                        CookieManager.getInstance()
                          .setAcceptThirdPartyCookies(v, allowThirdPartyCookies(url));
                        onUrlChanged(id, url);
                        onNavState(id, v.canGoBack(), v.canGoForward());
                        // As early as a plain WebView allows. It is not before the
                        // page's own inline scripts -- androidx.webkit's
                        // addDocumentStartJavaScript is what that would take --
                        // and android_view.h says so rather than implying document-start.
                        String js = injectedScripts(id);
                        if (js != null && !js.isEmpty())
                            v.evaluateJavascript(js, null);
                        // A new document has no listeners and no elements, so
                        // whatever was playing here is not playing now. Said
                        // before the watcher goes in, so a navigation away from
                        // a video drops the notification rather than stranding
                        // it on a page that no longer exists.
                        notePlaying(id, false);
                        v.evaluateJavascript(MEDIA_WATCH, null);
                    }

                    // Asked about every navigation, and silence is consent: a
                    // WebView that is not told otherwise will try to load
                    // magnet: itself and show its own error. renders_as_page()
                    // decides, in C++, so the desktop and this cannot drift.
                    @Override
                    public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest req) {
                        if (req == null || req.getUrl() == null)
                            return false;
                        final String url = req.getUrl().toString();
                        if (takeExternalUrl(url))
                            return true;
                        // A subframe navigating itself is not the page going
                        // anywhere, and a locked tab is about the page. Asking
                        // about one would spawn a sub-tab for an ad slot.
                        if (!req.isForMainFrame())
                            return false;
                        // True here means "handled, do not load", so a refusal
                        // is the negation of permission.
                        if (!allowNavigation(id, url, req.hasGesture()))
                            return true;
                        // Allowed, and the load has not started yet -- which is
                        // the only moment a document-start script for *this*
                        // url can still be registered. A link click never goes
                        // through load() above, so without this the shim would
                        // be armed for the previous page's origin.
                        armDocumentStart(id, v, url, null);
                        return false;
                    }

                    // Every subresource passes through here, on a network
                    // thread. Returning null means "load it normally", which is
                    // the answer for all but a few requests, so the cost of
                    // being on this path is one JNI call per request.
                    @Override
                    public WebResourceResponse shouldInterceptRequest(WebView v,
                                                                      WebResourceRequest req) {
                        if (req == null || req.getUrl() == null)
                            return null;
                        // The main document is not a subresource decision: a
                        // WebView that blocks its own page shows an empty page
                        // with no way back, and the rules are written about what
                        // a page loads, not about which page you may visit.
                        if (req.isForMainFrame())
                            return null;
                        String accept = "";
                        Map<String, String> headers = req.getRequestHeaders();
                        if (headers != null) {
                            // Header names are case-insensitive on the wire, and
                            // this map does not fold them, so look for both.
                            String a = headers.get("Accept");
                            if (a == null)
                                a = headers.get("accept");
                            if (a != null)
                                accept = a;
                        }
                        final String page = PAGE_URLS.get(Long.valueOf(id));
                        if (shouldBlock(req.getUrl().toString(), accept,
                                         page != null ? page : ""))
                            return blocked();
                        return null;
                    }
                });
                // Zero-sized until C++ says where the page area is; a view added
                // at full size would cover the toolbar for as long as it took
                // the first geometry to arrive, which is visible as a flash.
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(0, 0);
                a.addContentView(w, lp);
                // Above Qt's own surface. Added later is not enough on its own:
                // Qt draws into a SurfaceView, and a SurfaceView with Z-order
                // set punches through anything ordinary layered over it.
                w.bringToFront();
                w.setZ(100f);
                VIEWS.put(id, w);
            }
        });
    }

    /**
     * A WebView that can decline to notice it has been hidden.
     *
     * **Subclassed for one override, and only one.** A foreground service buys
     * the right to keep playing; it does not make the WebView want to.
     * Measured, with the service running and its notification on screen: a
     * video's audio still went from started to stopped two seconds after the
     * browser was backgrounded, while an audio-only stream on the same build
     * played on indefinitely. The difference is neither the service nor the
     * permission -- Chromium suspends a video whose window has become
     * invisible, and an audio element has nothing to suspend.
     * `onWindowVisibilityChanged` is where that is decided, so it is the one
     * place to decline it.
     */
    private static class Player extends WebView {
        /** A visibility that was withheld, or -1. */
        private int pending = -1;

        Player(android.content.Context c) { super(c); }

        @Override
        protected void onWindowVisibilityChanged(int visibility) {
            // **Only while a foreground service is held.** Without that
            // condition this is a browser that never stops decoding because it
            // happens to be on a page with a video, which is somebody's
            // battery. With it, the browser keeps playing exactly when it has
            // told the system it is playing, and the notification saying so is
            // on screen throughout.
            if (visibility != android.view.View.VISIBLE && SERVICE_UP) {
                pending = visibility;
                return;
            }
            pending = -1;
            super.onWindowVisibilityChanged(visibility);
        }

        /**
         * Hand over a withheld visibility once the reason for withholding it
         * has gone.
         *
         * Without this, a video that simply ends while the browser is hidden
         * leaves the engine believing its window is still on screen -- so the
         * renderer stays at foreground priority until somebody opens the
         * browser again, which is the battery cost the gate above exists to
         * avoid, arriving by the back door.
         */
        void settle() {
            if (pending == -1)
                return;
            final int v = pending;
            pending = -1;
            super.onWindowVisibilityChanged(v);
        }
    }

    /**
     * Which views are making a sound.
     *
     * **Why a script and not Android's own answer.** AudioManager will report
     * playback through registerAudioPlaybackCallback, but what an app receives
     * about players it does not own is anonymized, and a WebView's audio is not
     * separable from the browser's by that route -- it would say "something on
     * this device is playing" and this needs "this tab is playing". The page's
     * own media elements are the only thing that knows.
     *
     * **What it does not see:** media inside a cross-origin iframe, since
     * onPageStarted fires for the main frame only and the script goes in with
     * it. A video embedded in a frame therefore keeps the old behaviour rather
     * than gaining the new one, which is a smaller hole than it sounds -- the
     * sites this was reported against play in the main document.
     */
    private static final java.util.Set<Long> PLAYING = new java.util.HashSet<>();
    private static boolean SERVICE_UP = false;

    /**
     * Recomputed rather than counted.
     *
     * A play/pause counter drifts: a `pause` arrives for an element that was
     * already paused, an element is removed mid-play and its `ended` never
     * comes, and the count ends up permanently above zero holding a
     * notification over a silent browser. Asking the document how many
     * elements are actually playing cannot drift, because it is a fact rather
     * than a running total.
     *
     * The listeners are on the capture phase because media events do not
     * bubble -- `playing` on a `<video>` reaches a document listener only if
     * that listener is capturing. A bubble-phase listener here would compile,
     * install, and never fire once.
     */
    private static final String MEDIA_WATCH =
        "(function(){"
      + "if(window.__hydra_media)return;window.__hydra_media=1;"
      + "function s(){try{var m=document.querySelectorAll('video,audio'),a=false;"
      + "for(var i=0;i<m.length;i++){var e=m[i];"
      + "if(!e.paused&&!e.ended&&!e.muted&&e.readyState>2)a=true;}"
      + "hydraNative.mediaState(a);}catch(x){}}"
      + "var ev=['playing','pause','ended','emptied','volumechange'];"
      + "for(var i=0;i<ev.length;i++)document.addEventListener(ev[i],s,true);"
      // **Sampled as well as listened for, and this is not belt and braces.**
      // A cached video autoplays before this script is in -- injection is at
      // onPageStarted, which is not document-start -- so its `playing` event
      // has already been and gone and no later event says it is playing. That
      // is exactly what happened in the first measurement: audio running, and
      // the watcher reporting nothing until the clip ended. The immediate
      // sample catches what already started; the two delayed ones catch a
      // stream that has not reached readyState yet, and cost two timers on a
      // page load.
      + "s();setTimeout(s,700);setTimeout(s,3000);"
      + "})();";

    static void notePlaying(final long id, final boolean on) {
        onUi(new Runnable() {
            @Override public void run() {
                if (on)
                    PLAYING.add(id);
                else
                    PLAYING.remove(id);
                syncPlaybackService();
            }
        });
    }

    /**
     * Start the service while anything is playing, stop it when nothing is.
     *
     * **The order is what makes this legal.** From API 31 an app in the
     * background may not start a foreground service at all, so the start has to
     * happen while the browser is still in front -- which it is, because
     * playback begins with somebody pressing play. Backgrounding it afterwards
     * keeps a service that is already running, which is allowed.
     *
     * The consequence is a real limit rather than a bug: sound that begins
     * while the browser is already hidden cannot raise the service, and stopping
     * for a moment while backgrounded gives up the service until the browser is
     * looked at again.
     */
    private static void syncPlaybackService() {
        final Activity a = ACTIVITY;
        if (a == null)
            return;
        final boolean want = !PLAYING.isEmpty();
        if (want == SERVICE_UP)
            return;
        final android.content.Intent i =
            new android.content.Intent(a, PlaybackService.class);
        try {
            if (want) {
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O)
                    a.startForegroundService(i);
                else
                    a.startService(i);
            } else {
                a.stopService(i);
            }
            SERVICE_UP = want;
            if (!want)
                for (WebView v : VIEWS.values())
                    if (v instanceof Player)
                        ((Player) v).settle();
        } catch (RuntimeException e) {
            // A start refused because the app was in the background throws
            // rather than returning, and taking the browser down over a
            // notification would be a poor trade for the sound it was
            // protecting. SERVICE_UP is left alone so the next change tries
            // again.
        }
    }

    // The rest post to the view's own UI thread, which is the one that made it,
    // so they need no Activity at all.
    private static void onUi(final Runnable r) {
        if (ACTIVITY != null)
            ACTIVITY.runOnUiThread(r);
    }

    /**
     * Register the permissions shim to run before the next page's own scripts.
     *
     * **The whole point is the word "before".** onPageStarted, where this shim
     * used to go in alone, fires after the document's inline scripts have run.
     * Measured on the handset: the same page asking
     * `navigator.permissions.query({name:"camera"})` at parse time was told
     * "prompt" and asking again 2.5 seconds later was told "granted", from one
     * shim with one answer. Teams asks at parse time, concludes there is no
     * camera and says so, which is the report this path exists to fix.
     *
     * Called with the destination url rather than the current one, because a
     * document-start script is registered before the load and the shield's
     * answer is per site.
     *
     * **Feature-detected rather than assumed.** DOCUMENT_START_SCRIPT depends
     * on the WebView provider installed on the device, not on the androidx
     * dependency being compiled in, so a phone with an old provider takes the
     * onPageStarted path exactly as before -- late, but no worse than it was.
     */
    private static void armDocumentStart(long id, WebView w, String url,
                                          String provided) {
        if (w == null || url == null || url.isEmpty())
            return;
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            Log.d(PERM_TAG, "document-start unsupported by this WebView provider");
            return;
        }
        // Removed first and unconditionally: the handler outlives a navigation,
        // so leaving it would stack one script per page visited, each answering
        // with the origin it was built for.
        ScriptHandler old = START_SCRIPTS.remove(id);
        if (old != null) {
            try { old.remove(); } catch (RuntimeException e) { /* already gone */ }
        }
        // **Scoped to the origin the answer was computed for, not to "*".**
        //
        // The shield's answer is per site, and this is registered before the
        // navigation, so a redirect to somewhere else would run a script
        // carrying the *first* site's answer on the second site's document --
        // at parse time, which is exactly when it is believed. A site allowed
        // the camera redirecting to one that is blocked would have told the
        // second page "granted".
        //
        // Restricting the rule to this origin means such a document gets no
        // script at all and falls back to what the WebView says by itself,
        // which is the honest answer rather than the wrong one. onPageStarted
        // still injects afterwards with the url that actually loaded.
        //
        // A url with no host -- about:, data:, file: -- has no origin to scope
        // to and gets nothing; none of them is a site the shield has an
        // opinion about.
        final android.net.Uri u = android.net.Uri.parse(url);
        final String scheme = u.getScheme();
        final String host = u.getHost();
        if (scheme == null || host == null || host.isEmpty())
            return;
        final int port = u.getPort();
        final String rule = scheme + "://" + host + (port > 0 ? ":" + port : "");

        // A caller on the Qt thread passes the script; one on the UI thread
        // passes null and this asks, which is the round trip allow_navigation
        // already makes safely from here.
        String js = provided != null ? provided : documentStartScript(id, url);
        if (js == null || js.isEmpty()) {
            Log.d(PERM_TAG, "document-start: no script for " + url
                             + " (rule " + rule + ")");
            return;
        }
        try {
            START_SCRIPTS.put(id, WebViewCompat.addDocumentStartJavaScript(
                                      w, js, Collections.<String>singleton(rule)));
            Log.d(PERM_TAG, "document-start armed for " + rule
                             + " (" + js.length() + " chars)");
        } catch (IllegalArgumentException e) {
            // A rule the provider will not accept is not worth failing a
            // navigation over; the onPageStarted path still runs.
            Log.d(PERM_TAG, "document-start refused: " + e.getMessage());
        }
    }

    /**
     * Load a url, with the document-start script the C++ side already built.
     *
     * **Handed over rather than fetched back.** This called
     * documentStartScript() from the UI thread, which asks the Qt thread and
     * waits; at startup that timed out every time, because the Qt thread is
     * bringing the window up and not yet servicing queued calls. The page then
     * loaded with no shim, which is exactly the failure the shim exists to
     * prevent. The caller is already on the Qt thread and can read the shield
     * there, so it builds the script and passes it.
     */
    public static void load(final long id, final String url, final String startJs) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w != null) {
                    armDocumentStart(id, w, url, startJs);
                    w.loadUrl(url);
                }
            }
        });
    }

    public static void setGeometry(final long id, final int x, final int y,
                                   final int width, final int height) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w == null)
                    return;
                FrameLayout.LayoutParams lp =
                    new FrameLayout.LayoutParams(Math.max(0, width), Math.max(0, height));
                lp.leftMargin = x;
                lp.topMargin = y;
                w.setLayoutParams(lp);
                w.requestLayout();
            }
        });
    }

    public static void setVisible(final long id, final boolean on) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w == null)
                    return;
                w.setVisibility(on ? android.view.View.VISIBLE
                                   : android.view.View.GONE);
            }
        });
    }

    /**
     * The shell's per-site content policy, applied to this WebView.
     *
     * The same five booleans go to both backends, so the meaning of each is
     * the desktop's -- qtwebengine_view::apply_settings sets the matching
     * QWebEngineSettings attribute for four of them, and this must not drift
     * from that or one browser would enforce a different rule from the other
     * under one name in the settings dialog.
     *
     * Posted, like everything else here. WebSettings belongs to the WebView
     * and may only be touched on the thread that made it; asking it anything
     * would mean blocking the Qt thread on Android's UI thread, which is the
     * deadlock this app has already met once.
     */
    public static void applySettings(final long id, final boolean javascript,
                                     final boolean images, final boolean autoplay,
                                     final boolean popups, final boolean scrollbars) {
        onUi(new Runnable() { @Override public void run() {
            WebView w = VIEWS.get(id);
            if (w == null)
                return;
            WebSettings s = w.getSettings();
            // Turning this off also stops the bridge shim and every content
            // script, exactly as it does on the desktop: those are page
            // javascript, and a page with javascript off has none.
            s.setJavaScriptEnabled(javascript);
            // Both, and in this order. setBlockNetworkImage covers network
            // images alone; setLoadsImagesAutomatically covers every image,
            // which is what the desktop's AutoLoadImages means. Re-enabling
            // the second is documented to fetch what was blocked, so the
            // unblock has to already be in place when it runs or the images
            // stay missing until the next load -- the shape of bug that
            // reads as "the setting does nothing" from the other side of the
            // screen.
            s.setBlockNetworkImage(!images);
            s.setLoadsImagesAutomatically(images);
            // Inverted, and the inversion is the whole difference: the shell
            // asks whether autoplay is allowed, Android asks whether a
            // gesture is required.
            s.setMediaPlaybackRequiresUserGesture(!autoplay);
            // The counterpart of QWebEngineSettings::JavascriptCanOpenWindows.
            //
            // setSupportMultipleWindows is deliberately NOT set alongside it,
            // and this is the one thing in this method left undone on
            // purpose. With it on, window.open stops being a navigation and
            // becomes a WebChromeClient.onCreateWindow callback; this class
            // implements none, and an unhandled onCreateWindow drops the
            // request without a word. "Popups allowed" would then mean
            // "popups silently discarded" -- a setting that does the
            // opposite of what it says, which is worse than the gap it would
            // close. Left off, an allowed window.open loads in this same
            // WebView: not the desktop's new tab under the page that asked,
            // but the navigation does happen, and it goes through
            // shouldOverrideUrlLoading so the shell's navigation decider
            // still sees it. The real fix is new_window_requested wired
            // through to the shell from onCreateWindow, which is a piece of
            // work rather than a flag.
            s.setJavaScriptCanOpenWindowsAutomatically(popups);
            // On the View rather than on WebSettings, because Android has no
            // WebSettings equivalent of ShowScrollBars. Same meaning as Qt's:
            // the indicator goes away and the page still scrolls, which is
            // what kiosk mode is asking for (architecture doc sec 8).
            w.setVerticalScrollBarEnabled(scrollbars);
            w.setHorizontalScrollBarEnabled(scrollbars);
        } });
    }

    /**
     * Ask this view's pages to treat it as a desktop browser.
     *
     * The string is built on the C++ side from whatever the WebView offers, so
     * the two rules that make it a desktop one live beside the rules that
     * corrected it in the first place rather than being written out twice here.
     *
     * A reload follows because the user agent is read when a page is fetched:
     * without it the toggle appears to do nothing until the next navigation,
     * which is the shape of bug that gets reported as "the setting is broken".
     *
     * **And it reloads what was asked for, not what was arrived at.** The whole
     * reason to want this is a site that redirected because of the old string:
     * teams.microsoft.com sends a mobile user agent to
     * /v2/unsupported-browser, so by the time the toggle is used the tab is
     * sitting on the refusal, and a plain reload fetches the refusal again with
     * nothing appearing to change. getOriginalUrl is the url before the
     * redirects, which is exactly the one to ask for a second time -- and with
     * the desktop string in place it will not redirect.
     *
     * Falls back to reload() when there was no redirect, when the WebView has
     * no original url to give, or when the two are the same.
     */
    public static void setDesktopSite(final long id, final boolean on,
                                       final String url, final String startJs) {
        onUi(new Runnable() { @Override public void run() {
            WebView w = VIEWS.get(id);
            if (w == null)
                return;
            WebSettings s = w.getSettings();
            String base = correctedUserAgent(s.getUserAgentString());
            String ua = on ? desktopUserAgent(base) : base;
            if (ua != null && !ua.isEmpty())
                s.setUserAgentString(ua);
            // **A desktop string without a desktop viewport is half a
            // toggle**, and the half that was missing is the one a page can
            // see. Both of these default to false, and neither was ever set:
            // so a site told to serve its desktop pages -- which carry no
            // `width=device-width` viewport meta, having no reason to -- was
            // laid out at the device's own CSS width instead of at the wide
            // default a desktop layout is written for. On the handset this is
            // tested on that width is 320 CSS pixels, folded.
            //
            // setUseWideViewPort makes the WebView honour a viewport meta tag
            // and fall back to the wide default when there is none;
            // setLoadWithOverviewMode then scales that width down to fit
            // rather than opening zoomed into its top-left corner. Chrome's
            // own Request Desktop Site changes the layout viewport as well as
            // the string, which is why its desktop pages arrive looking like
            // desktop pages.
            //
            // Scoped to the toggle rather than set for every page: switching
            // the viewport model for ordinary mobile browsing is a change to
            // how every site lays out, and is not what this control means.
            s.setUseWideViewPort(on);
            s.setLoadWithOverviewMode(on);
            String original = w.getOriginalUrl();
            String now = w.getUrl();
            String target =
                (original != null && !original.isEmpty() && !original.equals(now))
                    ? original : now;
            // **Re-arm before reloading, because the script answers questions
            // whose answer just changed.** The document-start script is built
            // on the C++ side from what this view is at the time, and one of
            // the things it is built from is whether this view is claiming to
            // be a desktop -- the camera shape shim reads exactly that. The
            // script registered for the page now on screen was built when the
            // answer was the other one, and a reload does not go back through
            // `load()`, so without this the page comes back with the desktop
            // user agent and the mobile script.
            //
            // Measured before it was fixed: a site with `desktopSite:allow`
            // reloaded into the desktop string -- `X11; Linux x86_64`, no
            // "Mobile" -- and `getUserMedia` still answered 480x640 portrait,
            // because the shim that would have asked for landscape was not in
            // the script that came back with it.
            //
            // `null` for the script, so this asks the Qt thread for a fresh
            // one. That round trip is the one `load()` warns about at startup;
            // this is not startup, it is a policy answer arriving for a view
            // that is already up, and it is the same ask the link-click path
            // in `allow_navigation` already makes safely from this thread.
            // The url and the script both come from the caller: `w.getUrl()`
            // is empty part-way through a navigation, and `armDocumentStart`
            // gives up on an empty url before it logs anything, so asking
            // here failed silently and left the stale script in place.
            armDocumentStart(id, w, (url != null && !url.isEmpty()) ? url : target,
                              startJs);
            if (target != null && !target.equals(now))
                w.loadUrl(target);
            else
                w.reload();
        } });
    }

    /**
     * Page zoom, as a percentage, where 100 is unzoomed.
     *
     * **setInitialScale and not setTextZoom.** The desktop's set_zoom_factor
     * is QWebEngineView::setZoomFactor, which scales the whole page -- text,
     * images and layout boxes alike -- while setTextZoom scales text and
     * leaves everything else where it was. Picking that one would make the
     * same Ctrl+= mean two different things depending on which backend was
     * built, which is exactly what the seam exists to prevent.
     *
     * What does not carry over is reflow, and web_view_backend.h calls this
     * "reflow zoom": Qt re-lays the page out at the new scale, whereas
     * Android's initial scale is a viewport scale, so a zoomed page here can
     * need sideways scrolling where the desktop's would have rewrapped. A
     * platform difference, stated rather than papered over.
     *
     * **100 becomes 0, and no other value is taken literally.** 0 is
     * Android's "use the default". The scale is otherwise in device pixels,
     * so a literal setInitialScale(100) means one CSS pixel per device pixel
     * and draws the page at a third of its size on a 3x phone -- so every
     * other value is taken relative to the display density, which is what the
     * default scale is built from.
     *
     * **Unverified: whether this takes effect on a page already loaded.**
     * Android documents setInitialScale as the *initial* scale and says
     * nothing about a page that is already up; the desktop's setZoomFactor
     * rescales immediately. So Zoom In may do nothing here until the next
     * navigation, and that has not been driven on a device -- an attempt to
     * reach the View menu over adb kept leaving the app instead. Written down
     * rather than assumed either way: someone with the phone in their hand
     * settles it in ten seconds, and if it is true the answer is to reload, or
     * to move to WebSettingsCompat once androidx.webkit is a dependency.
     */
    public static void setZoomFactor(final long id, final int percent) {
        onUi(new Runnable() { @Override public void run() {
            WebView w = VIEWS.get(id);
            if (w == null)
                return;
            if (percent == 100) {
                w.setInitialScale(0);
                return;
            }
            float density = w.getResources().getDisplayMetrics().density;
            int base = Math.round(density * 100f);
            w.setInitialScale(Math.max(1, Math.round(base * percent / 100f)));
        } });
    }

    /**
     * Empty the cookie jar and every origin's site data.
     *
     * `WebStorage.deleteAllData()` goes with the cookies deliberately: it
     * covers localStorage, IndexedDB and WebSQL, which is more than the
     * desktop backend can clear at all, and leaving it out would mean a user
     * who cleared cookies on the phone kept the half of their identity that
     * lives in localStorage without being told.
     *
     * The flush is here and not in the general path: this is the one moment
     * when the on-disk jar must agree with memory immediately, because the
     * next thing the user does may be to close the app and check.
     */
    public static void clearCookiesAndSiteData() {
        onUi(new Runnable() { @Override public void run() {
            WebStorage.getInstance().deleteAllData();
            CookieManager.getInstance().removeAllCookies(
              new ValueCallback<Boolean>() {
                @Override public void onReceiveValue(Boolean removed) {
                    CookieManager.getInstance().flush();
                    onCookiesCleared(removed != null && removed.booleanValue());
                }
            });
        } });
    }

    /**
     * Empty the http cache of every open WebView.
     *
     * `clearCache(true)` is a method on a view rather than on a manager, so
     * with no view open there is nothing to call it on -- the C++ side checks
     * that first and reports the request refused instead of pretending. There
     * is no completion callback of any kind, which is why the report says
     * `unconfirmed` however well this goes.
     */
    public static void clearCache() {
        onUi(new Runnable() { @Override public void run() {
            for (WebView w : VIEWS.values())
                w.clearCache(true);
        } });
    }

    public static void back(final long id) { nav(id, -1); }
    public static void forward(final long id) { nav(id, 1); }

    /**
     * Reload, re-arming the document-start script first.
     *
     * **A reload is a navigation, and the script answers questions whose
     * answers change between them.** The shim carries the shield's camera and
     * microphone answers and whether this view is claiming to be a desktop,
     * all baked in as text when it was armed. The reported journey is the one
     * that breaks: a site says it cannot find the camera, the shield is opened
     * and set to allow, `on_policy_changed` saves and reloads -- and the page
     * comes back with the *old* answer still in the script, so it says the
     * same thing again. The late `onPageStarted` injection does carry the new
     * answer and arrives after the page's own inline scripts, which is the
     * exact failure the document-start path exists to prevent.
     *
     * The desktop does not have this: `navigating_page::acceptNavigationRequest`
     * rebuilds its shim for every main-frame navigation, which is one hook
     * covering every route. Android arms at named call sites instead, so each
     * route has to be remembered -- `load`, `shouldOverrideUrlLoading` and
     * `setDesktopSite` were remembered and these two were not.
     */
    public static void reload(final long id, final String url, final String startJs) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w == null)
                    return;
                armDocumentStart(id, w, url, startJs);
                w.reload();
            }
        });
    }

    /**
     * Back or forward, re-arming the document-start script for where we are
     * going rather than for where we have been.
     *
     * **The destination is known here and only here.** C++ cannot pass it in:
     * a history entry is the WebView's own state, and asking for it would be a
     * round trip to find out what to send on a round trip. `copyBackForwardList`
     * answers it directly, so the script is armed for the origin actually about
     * to load.
     *
     * Without this the handler stays scoped to the origin it was armed for, so
     * going back to a previously-visited site matched nothing and that page
     * loaded with no document-start script at all -- falling back to the late
     * `onPageStarted` injection, which is the arrangement this path exists to
     * replace. Degraded rather than wrong, and invisible, which is why it
     * survived: the page still worked, just with the shim arriving after its
     * own scripts had already asked.
     *
     * `null` for the script, so this asks the Qt thread for one built against
     * the destination. Same ask `shouldOverrideUrlLoading` already makes from
     * this thread.
     */
    private static void nav(final long id, final int dir) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w == null)
                    return;
                final boolean can = (dir < 0) ? w.canGoBack() : w.canGoForward();
                if (!can)
                    return;
                WebBackForwardList list = w.copyBackForwardList();
                WebHistoryItem to = (list != null)
                    ? list.getItemAtIndex(list.getCurrentIndex() + (dir < 0 ? -1 : 1))
                    : null;
                if (to != null && to.getUrl() != null && !to.getUrl().isEmpty())
                    armDocumentStart(id, w, to.getUrl(), null);
                if (dir < 0)
                    w.goBack();
                else
                    w.goForward();
            }
        });
    }

    /**
     * Stop whatever this view is loading.
     *
     * **The seam's `stop()` was the base class's empty one here**, so Reload
     * offered Stop, the shell hid the progress bar and said "Stopped.", and
     * the page carried on loading behind all three. That was invisible only
     * because the progress signals were missing too and the button never
     * became Stop; adding those without this would have made it a lie
     * somebody could see.
     */
    public static void stopLoading(final long id) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w != null)
                    w.stopLoading();
            }
        });
    }

    public static void destroy(final long id) {
        onUi(new Runnable() {
            @Override public void run() {
                PAGE_URLS.remove(Long.valueOf(id));
                // **Whatever this view was waiting to be answered goes too.**
                // A tab closed while a prompt is up leaves a request nobody
                // will ever answer, and the request holds frame state behind
                // it. Only geolocation can be attributed to a view here --
                // capture requests are keyed by token alone -- so this is the
                // half that can be done at destruction, and
                // `onPermissionRequestCanceled` is what covers the other.
                Long geo = GEO_TOKEN.remove(Long.valueOf(id));
                if (geo != null) {
                    GEO_PENDING.remove(geo);
                    GEO_ORIGIN.remove(geo);
                }
                WebView w = VIEWS.remove(id);
                if (w == null)
                    return;
                // A closed tab is silent whatever it was doing a moment ago.
                PLAYING.remove(id);
                syncPlaybackService();
                // **And it holds no document-start script.** The handler is
                // the WebView's, so it is dropped before the view is
                // destroyed rather than after: removing one from a destroyed
                // WebView is not something the API promises to survive. Left
                // in the map it would be both a leak and a stale entry that
                // the next view to take this id would try to remove.
                ScriptHandler gone = START_SCRIPTS.remove(id);
                if (gone != null) {
                    try { gone.remove(); } catch (RuntimeException e) { /* already gone */ }
                }
                ViewGroup p = (ViewGroup) w.getParent();
                if (p != null)
                    p.removeView(w);
                w.destroy();
            }
        });
    }
}
