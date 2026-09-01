package se.vibes.hydra;

import android.app.Activity;
import android.graphics.Color;
import android.net.Uri;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.WebStorage;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.ValueCallback;
import android.webkit.PermissionRequest;
import android.webkit.WebChromeClient;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.ByteArrayInputStream;
import java.util.HashMap;
import java.util.Map;

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
    private static final Map<Long, PermissionRequest> PENDING = new HashMap<>();
    private static long NEXT_TOKEN = 1;

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
                if (req == null)
                    return;
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
     * network thread reads it. Volatile because those are different threads and
     * a stale read would apply the previous page's rules.
     */
    private static volatile String PAGE_URL = "";

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
                        if (!video && !audio) {
                            req.deny();
                            return;
                        }
                        final long token = NEXT_TOKEN++;
                        PENDING.put(token, req);
                        final String origin = req.getOrigin() != null
                                ? req.getOrigin().toString() : "";
                        requestCapture(id, origin, video, audio, token);
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

                    @Override
                    public void onPageStarted(WebView v, String url, android.graphics.Bitmap f) {
                        PAGE_URL = url;
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
                        return !allowNavigation(id, url, req.hasGesture());
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
                        if (shouldBlock(req.getUrl().toString(), accept, PAGE_URL))
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

    public static void load(final long id, final String url) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w != null)
                    w.loadUrl(url);
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

    public static void reload(final long id) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w != null)
                    w.reload();
            }
        });
    }

    private static void nav(final long id, final int dir) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.get(id);
                if (w == null)
                    return;
                if (dir < 0 && w.canGoBack())
                    w.goBack();
                else if (dir > 0 && w.canGoForward())
                    w.goForward();
            }
        });
    }

    public static void destroy(final long id) {
        onUi(new Runnable() {
            @Override public void run() {
                WebView w = VIEWS.remove(id);
                if (w == null)
                    return;
                // A closed tab is silent whatever it was doing a moment ago.
                PLAYING.remove(id);
                syncPlaybackService();
                ViewGroup p = (ViewGroup) w.getParent();
                if (p != null)
                    p.removeView(w);
                w.destroy();
            }
        });
    }
}
