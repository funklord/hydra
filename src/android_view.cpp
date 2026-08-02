// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_view.h"
#include "request_filter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QJniEnvironment>
#include <QJniObject>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QTimer>
#include <functional>

namespace {

const char *k_cls = "org/qtproject/example/hydra/HydraWebView";

// The page-side half of the bridge: the same `window.hydraChannel(cb)` the
// desktop's QWebChannel bootstrap provides, so every injected script runs
// unmodified on both engines.
//
// The desktop's proxies are asynchronous — a call takes a trailing callback and
// the answer arrives later — because QWebChannel talks over a transport.
// `hydraNative` is a synchronous Java call, so the callback is invoked inline.
// That difference is invisible to a script written against the desktop shape,
// which is the point: the scripts are the contract, not the transport.
const char *k_bridge_bootstrap = R"JS(
(function () {
  if (window.hydraChannel || !window.hydraNative) return;
  var objects = {};
  var build = function (name) {
    var d;
    try { d = JSON.parse(window.hydraNative.describe(name)); } catch (e) { return null; }
    if (!d || !d.ok) return null;
    var proxy = {};
    d.value.methods.forEach(function (m) {
      proxy[m.name] = function () {
        var args = Array.prototype.slice.call(arguments);
        // A trailing function is the desktop's result callback, not an argument.
        var cb = (args.length > m.args && typeof args[args.length - 1] === 'function')
                   ? args.pop() : null;
        var out;
        try {
          out = JSON.parse(window.hydraNative.call(name, m.name, JSON.stringify(args)));
        } catch (e) { out = { ok: false }; }
        // A failed call calls nothing back, exactly as a dropped transport
        // message would: a script waiting on an answer waits, rather than
        // acting on a value the shell never produced.
        if (cb && out && out.ok) { try { cb(out.value); } catch (e) {} }
        return out && out.ok ? out.value : undefined;
      };
    });
    return proxy;
  };
  window.hydraChannel = function (cb) {
    try { cb(objects); } catch (e) {}
  };
  window.hydraRegisterBridge = function (name) {
    var p = build(name);
    if (p) objects[name] = p;
  };
})();
)JS";


// One id per view, so the Java side can keep more than one WebView and the C++
// side never holds a Java reference across threads.
qint64 next_id() {
	static qint64 n = 0;
	return ++n;
}

}  // namespace

// Called from Java when a load starts. Static, because JNI has nowhere to put a
// `this`, so the id is the handle back to the view that asked.
extern "C" JNIEXPORT void JNICALL
Java_org_qtproject_example_hydra_HydraWebView_onUrlChanged(JNIEnv *env, jclass,
                                                            jlong id, jstring url) {
	const char *utf = env->GetStringUTFChars(url, nullptr);
	const QString s = QString::fromUtf8(utf);
	env->ReleaseStringUTFChars(url, utf);
	android_view::report_url(id, s);
}

// Called from Java on the WebView's network thread, once per request. Returning
// true makes the Java side answer with an empty response, which is how a
// WebView blocks: there is no "cancel this request" to call.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_qtproject_example_hydra_HydraWebView_shouldBlock(JNIEnv *env, jclass,
                                                           jstring url, jstring accept,
                                                           jstring page_url) {
	const auto pull = [env](jstring s) {
		if (!s)
			return QString();
		const char *utf = env->GetStringUTFChars(s, nullptr);
		const QString out = QString::fromUtf8(utf);
		env->ReleaseStringUTFChars(s, utf);
		return out;
	};
	return android_view::should_block(pull(url), pull(accept), pull(page_url))
	           ? JNI_TRUE : JNI_FALSE;
}


namespace {

// Runs `fn` on the Qt thread and waits for its answer. These arrive on a binder
// thread that Android owns; the bridges are ordinary QObjects living on the Qt
// thread, so this is the boundary rather than making every bridge thread-safe.
// No deadlock: the Qt thread never blocks waiting on a binder thread.
QString on_qt_thread(std::function<QString()> fn) {
	QString out;
	QMetaObject::invokeMethod(qApp, [&] { out = fn(); }, Qt::BlockingQueuedConnection);
	return out;
}

// A QString as a javascript string literal, quoted and escaped by the JSON
// writer rather than by hand: these names come from the shell, but building
// javascript by concatenation is how the next one gets in.
QString js_literal(const QString &s) {
	const QByteArray j = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
	return QString::fromUtf8(j.mid(1, j.size() - 2));   // drop the [ ]
}

QString from_java(JNIEnv *env, jstring s) {
	if (!s)
		return QString();
	const char *utf = env->GetStringUTFChars(s, nullptr);
	const QString out = QString::fromUtf8(utf);
	env->ReleaseStringUTFChars(s, utf);
	return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_qtproject_example_hydra_HydraWebView_bridgeDescribe(JNIEnv *env, jclass,
                                                              jlong id, jstring name) {
	const QString n = from_java(env, name);
	const QString r = on_qt_thread([id, n] { return android_view::describe_bridge(id, n); });
	return env->NewStringUTF(r.toUtf8().constData());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_qtproject_example_hydra_HydraWebView_bridgeCall(JNIEnv *env, jclass, jlong id,
                                                          jstring name, jstring method,
                                                          jstring args) {
	const QString n = from_java(env, name), m = from_java(env, method),
	              a = from_java(env, args);
	const QString r =
		on_qt_thread([id, n, m, a] { return android_view::call_bridge(id, n, m, a); });
	return env->NewStringUTF(r.toUtf8().constData());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_qtproject_example_hydra_HydraWebView_injectedScripts(JNIEnv *env, jclass,
                                                               jlong id) {
	const QString r = on_qt_thread([id] { return android_view::injected_scripts(id); });
	return env->NewStringUTF(r.toUtf8().constData());
}

QString android_view::describe_bridge(qint64 id, const QString &name) {
	android_view *v = s_views.value(id);
	return v ? v->m_bridges.describe(name)
	         : QStringLiteral(R"({"ok":false,"error":"no such view"})");
}

QString android_view::call_bridge(qint64 id, const QString &name, const QString &method,
                                   const QString &args_json) {
	android_view *v = s_views.value(id);
	return v ? v->m_bridges.invoke(name, method, args_json)
	         : QStringLiteral(R"({"ok":false,"error":"no such view"})");
}

QString android_view::injected_scripts(qint64 id) {
	android_view *v = s_views.value(id);
	if (!v)
		return QString();
	// Shim first, then the registered bridges, then the scripts that use them.
	// A script that ran before its bridge existed would see hydraChannel hand it
	// an object without the method it wants, which is the kind of failure that
	// looks like a bug in the script.
	QString out = QString::fromUtf8(k_bridge_bootstrap);
	for (const QString &name : v->m_bridges.names())
		out += QStringLiteral("\nwindow.hydraRegisterBridge(%1);\n").arg(js_literal(name));
	for (const QString &src : v->m_script_sources)
		out += "\n;(function(){\n" + src + "\n})();\n";
	return out;
}

QHash<qint64, android_view *> android_view::s_views;
request_filter *android_view::s_filter = nullptr;

bool android_view::should_block(const QString &url, const QString &accept,
                                 const QString &page_url) {
	if (!s_filter)
		return false;

	request_context ctx;
	ctx.url          = QUrl(url);
	ctx.request_host = ctx.url.host();
	// The page's own host. Java reads it from the WebView on the UI thread and
	// caches it, because this call arrives on a thread that may not touch the
	// view at all. A blank one means a request racing the first navigation, and
	// leaving site_host empty is right: no per-site rule matches, so only the
	// rules that key off the request host apply, which is the safe subset.
	ctx.site_host    = QUrl(page_url).host();
	ctx.kind         = kind_from_hints(accept, ctx.url);

	const request_decision d = s_filter->decide(ctx);
	// Observers see every request here too, exactly as on the desktop: the media
	// detector wants what loaded and filter-evolution wants what got through.
	s_filter->notify(ctx, d);
	// d.strip_referer is deliberately dropped. See android_view.h: this hook can
	// replace a response but not edit a request, and silently doing nothing is
	// better than pretending. It is not lost -- the desktop honours it, and the
	// decision is one struct for both.
	return d.block;
}

void android_view::report_url(qint64 id, const QString &url) {
	// Onto the Qt thread: this arrives on Android's UI thread.
	QMetaObject::invokeMethod(qApp, [id, url] {
		if (android_view *v = s_views.value(id))
			v->on_url_from_java(url);
	}, Qt::QueuedConnection);
}

void android_view::on_url_from_java(const QString &url) {
	if (url == m_url.toString())
		return;
	m_url = QUrl(url);
	emit url_changed(m_url);
}

android_view::android_view(request_filter *filter, QWidget *parent)
    : web_view_backend(nullptr) {
	s_filter = filter;
	m_widget = new QLabel(parent);
	m_widget->setObjectName("android_placeholder");
	m_widget->setWordWrap(true);
	m_widget->setAlignment(Qt::AlignCenter);
	m_widget->setMargin(24);
	// The backend is owned by the widget, the same way the desktop one is, so
	// the shell can delete the widget and be done.
	setParent(m_widget);
	refresh();

	// Ask Java for a WebView. If the class is missing -- an APK built without
	// the android/ package source dir, say -- everything below is skipped and
	// the placeholder above is what the user sees. A backend that half-worked
	// would be worse than one that plainly does not.
	//
	// On by default, because it works: pages load over plain http and https,
	// links navigate, back returns, and the address bar follows. It was opt-in
	// while it painted nothing and while Android's cleartext policy blocked
	// every http:// address, and both of those are fixed. `HYDRA_ANDROID_WEBVIEW=0`
	// puts the placeholder back, which is worth keeping for anyone bisecting a
	// WebView bug against the rest of the shell.
	if (qEnvironmentVariableIsSet("HYDRA_ANDROID_WEBVIEW")
	    && qEnvironmentVariableIntValue("HYDRA_ANDROID_WEBVIEW") == 0)
		return;

	m_id = next_id();
	s_views.insert(m_id, this);
	QJniObject::callStaticMethod<void>(
		k_cls, "create", "(Landroid/app/Activity;J)V",
		QNativeInterface::QAndroidApplication::context().object(), jlong(m_id));
	// A missing class throws rather than returning anything, so the exception
	// state is the answer to "is there a WebView".
	m_native = !QJniEnvironment().checkAndClearExceptions();
	if (m_native) {
		m_widget->installEventFilter(this);
		// The widget is only a stand-in for the page area now, so it should not
		// be showing an explanation of itself behind a live WebView.
		m_widget->clear();
	}
}

android_view::~android_view() {
	s_views.remove(m_id);
	if (m_native)
		QJniObject::callStaticMethod<void>(k_cls, "destroy", "(J)V", jlong(m_id));
}

bool android_view::eventFilter(QObject *o, QEvent *e) {
	if (o == m_widget) {
		switch (e->type()) {
			case QEvent::Resize:
			case QEvent::Move:
			case QEvent::Show:
				sync_geometry();
				break;
			case QEvent::Hide:
				if (m_native)
					QJniObject::callStaticMethod<void>(k_cls, "setVisible", "(JZ)V",
					                                    jlong(m_id), jboolean(false));
				break;
			default:
				break;
		}
	}
	return web_view_backend::eventFilter(o, e);
}

void android_view::sync_geometry() {
	if (!m_native || !m_widget)
		return;
	// In *device* pixels: Qt reports logical ones and Android's layout wants
	// physical, and on a phone the two differ by a factor of three.
	const qreal dpr = m_widget->devicePixelRatioF();
	const QPoint top_left = m_widget->mapTo(m_widget->window(), QPoint(0, 0));
	QJniObject::callStaticMethod<void>(k_cls, "setGeometry", "(JIIII)V",
	                                    jlong(m_id),
	                                    jint(top_left.x() * dpr),
	                                    jint(top_left.y() * dpr),
	                                    jint(m_widget->width() * dpr),
	                                    jint(m_widget->height() * dpr));
	QJniObject::callStaticMethod<void>(k_cls, "setVisible", "(JZ)V",
	                                    jlong(m_id), jboolean(m_widget->isVisible()));
}

QWidget *android_view::widget() {
	return m_widget;
}

void android_view::refresh() {
	m_widget->setText(
		QStringLiteral(
			"<h2>The web view is turned off</h2>"
			"<p>Everything else in Hydra is running: the tree, the policy "
			"engine, the download queue and the request filter are the same "
			"code as the desktop build.</p>"
			"<p>Hydra normally uses the System WebView here, behind "
			"<tt>web_view_backend</tt>. It is off because "
			"<tt>HYDRA_ANDROID_WEBVIEW=0</tt> was set, or because this APK was "
			"built without the <tt>android/</tt> package source directory and "
			"so has no <tt>HydraWebView</tt> class to talk to.</p>%1")
			.arg(m_url.isEmpty()
			         ? QString()
			         : QStringLiteral("<p>It was asked to open:<br><tt>%1</tt></p>")
			               .arg(m_url.toString().toHtmlEscaped())));
}

void android_view::load(const QUrl &url) {
	m_url = url;
	if (!m_native) {
		// No WebView: say so, and say what was asked for.
		refresh();
		emit url_changed(url);
		return;
	}
	sync_geometry();
	QJniObject::callStaticMethod<void>(
		k_cls, "load", "(JLjava/lang/String;)V", jlong(m_id),
		QJniObject::fromString(url.toString()).object<jstring>());
	emit url_changed(url);
}

void android_view::back() {
	if (m_native)
		QJniObject::callStaticMethod<void>(k_cls, "back", "(J)V", jlong(m_id));
}

void android_view::forward() {
	if (m_native)
		QJniObject::callStaticMethod<void>(k_cls, "forward", "(J)V", jlong(m_id));
}

void android_view::reload() {
	if (m_native)
		QJniObject::callStaticMethod<void>(k_cls, "reload", "(J)V", jlong(m_id));
}

void android_view::inject_script(const QString &name, const QString &source,
                                  bool subframes) {
	// `subframes` is not honoured and cannot be: evaluateJavascript runs in the
	// main frame. A script that only matters in an iframe -- the consent one --
	// therefore sees less here than on the desktop, which is a gap to close with
	// per-frame injection, not a flag to pretend about.
	Q_UNUSED(subframes)
	m_script_names << name;
	m_script_sources << source;
}

void android_view::inject_main_world_script(const QString &name,
                                             const QString &source) {
	// The same thing here. Android has one world, so the distinction the desktop
	// draws between an isolated script and a main-world one does not exist, and
	// both land in the page's own globals.
	m_script_names << name;
	m_script_sources << source;
}

void android_view::set_script_bridge(QObject *object, const QString &name) {
	m_bridges.add(name, object);
}

QByteArray android_view::save_state() const {
	// The address is all there is to keep. A real backend saves the WebView's
	// own back-forward list here.
	return m_url.toEncoded();
}

bool android_view::restore_state(const QByteArray &blob) {
	if (blob.isEmpty())
		return false;
	m_url = QUrl::fromEncoded(blob);
	refresh();
	return true;
}

web_view_backend *android_factory::create_view(QWidget *parent) {
	return new android_view(m_filter, parent);
}
