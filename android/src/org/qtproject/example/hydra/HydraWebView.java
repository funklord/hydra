// SPDX-License-Identifier: GPL-3.0-or-later
package org.qtproject.example.hydra;

import android.app.Activity;
import android.graphics.Color;
import android.view.ViewGroup;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

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
                WebView w = new WebView(a);
                w.getSettings().setJavaScriptEnabled(true);
                w.getSettings().setDomStorageEnabled(true);
                w.setBackgroundColor(Color.WHITE);
                // Software layer, deliberately. This view is composited on top
                // of Qt's own hardware surface, and with the default hardware
                // layer the renderer process died immediately -- measured, and
                // isolated by the stock WebView Browser Tester rendering the
                // same page on the same emulator without a murmur. Slower, and
                // it is the difference between a page and a blank rectangle.
                w.setLayerType(android.view.View.LAYER_TYPE_SOFTWARE, null);
                w.setWebViewClient(new WebViewClient() {
                    @Override
                    public void onPageStarted(WebView v, String url, android.graphics.Bitmap f) {
                        onUrlChanged(id, url);
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
                ViewGroup p = (ViewGroup) w.getParent();
                if (p != null)
                    p.removeView(w);
                w.destroy();
            }
        });
    }
}
