// **Guarded here, not only by the build system.** `hydra.pro` drops this file
// outside `android {}`, and until it also said so itself the file depended on a
// build file to be correct: anything reading the tree rather than the build --
// an indexer, a language server, a static analyser, `fmake` -- reached
// `<QJniEnvironment>` on a desktop and stopped. Measured, not imagined.
//
// `<QtGlobal>` first, and it is load-bearing: the compiler defines `__ANDROID__`
// but **`Q_OS_ANDROID` is Qt's**, so testing it before any Qt header is included
// is false everywhere -- including on Android, where it would empty this file in
// the one build that needs it.
#include <QtGlobal>
#ifdef Q_OS_ANDROID

#include "android_view.h"
#include "request_filter.h"
#include "permissions_shim.h"
#include "scheme_rules.h"
#include "user_agent.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QLabel>
#include <QJniEnvironment>
#include <QJniObject>
#include <QPermissions>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QTimer>
#include <QSemaphore>
#include <functional>
#include <memory>

namespace {

const char *k_cls = "se/vibes/hydra/HydraWebView";

// The page-side half of the bridge: the same `window.hydraChannel(cb)` the
// desktop's QWebChannel bootstrap provides, so every injected script runs
// unmodified on both engines.
//
// The desktop's proxies are asynchronous -- a call takes a trailing callback and
// the answer arrives later -- because QWebChannel talks over a transport.
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
Java_se_vibes_hydra_HydraWebView_onUrlChanged(JNIEnv *env, jclass,
                                                            jlong id, jstring url) {
	const char *utf = env->GetStringUTFChars(url, nullptr);
	const QString s = QString::fromUtf8(utf);
	env->ReleaseStringUTFChars(url, utf);
	android_view::report_url(id, s);
}

extern "C" JNIEXPORT void JNICALL
Java_se_vibes_hydra_HydraWebView_onNavState(JNIEnv *, jclass, jlong id,
                                             jboolean back, jboolean forward) {
	android_view::report_nav_state(id, back == JNI_TRUE, forward == JNI_TRUE);
}

// Called from Java on the WebView's network thread, once per request. Returning
// true makes the Java side answer with an empty response, which is how a
// WebView blocks: there is no "cancel this request" to call.
extern "C" JNIEXPORT jboolean JNICALL
Java_se_vibes_hydra_HydraWebView_shouldBlock(JNIEnv *env, jclass,
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

extern "C" JNIEXPORT jboolean JNICALL
Java_se_vibes_hydra_HydraWebView_allowThirdPartyCookies(JNIEnv *env, jclass,
                                                                      jstring page_url) {
	QString host;
	if (page_url) {
		const char *utf = env->GetStringUTFChars(page_url, nullptr);
		host = QString::fromUtf8(utf);
		env->ReleaseStringUTFChars(page_url, utf);
	}
	return android_view::allow_third_party_cookies(host) ? JNI_TRUE : JNI_FALSE;
}


namespace {

// Runs `fn` on the Qt thread and waits for its answer -- **but not for ever, and
// the old comment here claimed a safety that does not hold.**
//
// It said: "No deadlock: the Qt thread never blocks waiting on a binder thread."
// Two things were wrong with that. Some of these calls arrive on Android's *UI*
// thread rather than a binder thread, and the Qt thread does block on the UI
// thread -- inside ordinary repainting, where `QOpenGLContext::makeCurrent`
// waits for a surface the UI thread services. A navigation landing while Qt was
// mid-flush left each waiting for the other, and ten seconds later Android put
// up "Hydra isn't responding". The ANR trace showed both halves.
//
// So: posted, then waited on with a deadline. A caller that times out gets its
// own safe default rather than the browser's input queue. The result is held by
// `shared_ptr` because the lambda may still run after the wait gives up, and it
// must not write into a stack frame that has gone.
//
// **A deadline is a mitigation, not a fix.** The fix is not to ask, which is
// what `takeExternalUrl` now does -- see `claims_external_url`. This exists for
// the questions that genuinely need the Qt thread's state.
bool on_qt_thread(std::function<QString()> fn, QString *out, int ms = 2000) {
	auto done = std::make_shared<QSemaphore>();
	auto res  = std::make_shared<QString>();
	QMetaObject::invokeMethod(
	  qApp, [fn, done, res] { *res = fn(); done->release(); },
	  Qt::QueuedConnection);
	if (!done->tryAcquire(1, ms))
		return false;
	if (out)
		*out = *res;
	return true;
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
Java_se_vibes_hydra_HydraWebView_bridgeDescribe(JNIEnv *env, jclass,
                                                              jlong id, jstring name) {
	const QString n = from_java(env, name);
	QString r;
	// An empty description on timeout, which is what a bridge that does not
	// exist already returns -- so the page sees "no such bridge" rather than
	// hanging on a promise nothing will resolve.
	on_qt_thread([id, n] { return android_view::describe_bridge(id, n); }, &r);
	return env->NewStringUTF(r.toUtf8().constData());
}

extern "C" JNIEXPORT jstring JNICALL
Java_se_vibes_hydra_HydraWebView_bridgeCall(JNIEnv *env, jclass, jlong id,
                                                          jstring name, jstring method,
                                                          jstring args) {
	const QString n = from_java(env, name), m = from_java(env, method),
	              a = from_java(env, args);
	QString r;
	on_qt_thread([id, n, m, a] { return android_view::call_bridge(id, n, m, a); }, &r);
	return env->NewStringUTF(r.toUtf8().constData());
}

// Called from Java on the UI thread, before every navigation the WebView is
// about to make. True means the shell took it.
// Called from Java on the UI thread when a page's file input is used. Posted
// rather than waited on: the picker Qt is about to show needs that same UI
// thread, so blocking here would deadlock the two against each other.
// Queued, exactly like `requestCapture` and for the same reason: answering may
// put a dialog on the screen, and this arrives on Android's UI thread.
extern "C" JNIEXPORT void JNICALL
Java_se_vibes_hydra_HydraWebView_requestGeolocation(JNIEnv *env, jclass, jlong id,
                                                                  jstring origin,
                                                                  jlong token) {
	const QString o = from_java(env, origin);
	const qint64 vid = id, tok = token;
	QMetaObject::invokeMethod(qApp, [vid, o, tok] {
		android_view::request_geolocation(vid, o, tok);
	}, Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
Java_se_vibes_hydra_HydraWebView_chooseFile(JNIEnv *env, jclass, jlong id,
                                                          jboolean multiple,
                                                          jstring accept) {
	const QString a = from_java(env, accept);
	const bool many = multiple == JNI_TRUE;
	QMetaObject::invokeMethod(
	  qApp, [id, many, a] { android_view::choose_file(id, many, a); },
	  Qt::QueuedConnection);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_se_vibes_hydra_HydraWebView_takeExternalUrl(JNIEnv *env, jclass,
                                                               jstring url) {
	const QString u = from_java(env, url);
	// **Answered here, acted on there.** This used to hop to the Qt thread and
	// wait, on the UI thread, which is half of a deadlock -- see the note on
	// `claims_external_url`. The decision reads only the url, so it needs no
	// hop; the handler does, and is posted.
	if (!android_view::claims_external_url(u))
		return JNI_FALSE;
	QMetaObject::invokeMethod(
	  qApp, [u] { android_view::hand_to_external(u); }, Qt::QueuedConnection);
	return JNI_TRUE;
}

// Asked on Android's UI thread, once per main-frame navigation. The answer has
// to come from the Qt thread because the decider reads the tab tree and may
// queue a sub-tab, so it goes through `on_qt_thread` like the external-url
// question above it.
extern "C" JNIEXPORT jboolean JNICALL
Java_se_vibes_hydra_HydraWebView_allowNavigation(JNIEnv *env, jclass, jlong id,
                                                  jstring url, jboolean gesture) {
	const QString u = from_java(env, url);
	const bool user = gesture == JNI_TRUE;
	QString r;
	// **Allowed if the answer does not arrive**, which is the same default the
	// decider itself documents for a view with nobody listening: a refusal
	// nobody asked for is a browser that will not browse. This one still has to
	// ask -- the decision reads the tab tree and may queue a sub-tab -- so it
	// gets the deadline rather than the split.
	if (!on_qt_thread([id, u, user] {
		    return android_view::allow_navigation(id, u, user)
		               ? QStringLiteral("1")
		               : QString();
	    }, &r))
		return JNI_TRUE;
	return r.isEmpty() ? JNI_FALSE : JNI_TRUE;
}

// **Queued, not blocking, unlike every other entry point here.** Answering may
// put an Android permission dialog in front of the user, and `on_qt_thread`
// holds the calling binder thread until its answer arrives -- which for a
// dialog is until somebody taps it. The Java side was written to be answered
// later for exactly this reason, so nothing waits.
extern "C" JNIEXPORT void JNICALL
Java_se_vibes_hydra_HydraWebView_requestCapture(JNIEnv *env, jclass, jlong id,
                                                 jstring origin, jboolean video,
                                                 jboolean audio, jlong token) {
	const QString o     = from_java(env, origin);
	const bool    wantv = video == JNI_TRUE;
	const bool    wanta = audio == JNI_TRUE;
	const qint64  vid = id, tok = token;
	QMetaObject::invokeMethod(qApp, [vid, o, wantv, wanta, tok] {
		android_view::request_capture(vid, o, wantv, wanta, tok);
	}, Qt::QueuedConnection);
}

// The user agent, corrected by the same function the desktop uses.
//
// **No thread hop, and that is not an oversight.** `user_agent::corrected` is a
// pure function of the string it is given -- it touches no view, no shell and no
// Qt object -- so it can answer on whatever thread Java asks from. Every other
// entry point here either posts or waits with a deadline; this one needs
// neither, which is the cheapest kind of correct.
extern "C" JNIEXPORT jstring JNICALL
Java_se_vibes_hydra_HydraWebView_correctedUserAgent(JNIEnv *env, jclass,
                                                                  jstring offered) {
	const QString in = from_java(env, offered);
	if (in.isEmpty())
		return env->NewStringUTF("");
	return env->NewStringUTF(user_agent::corrected(in).toUtf8().constData());
}

// The desktop form of the string, for "request desktop site". Pure, like the
// correction beside it, so it needs no thread hop either.
extern "C" JNIEXPORT jstring JNICALL
Java_se_vibes_hydra_HydraWebView_desktopUserAgent(JNIEnv *env, jclass,
                                                                jstring mobile) {
	const QString in = from_java(env, mobile);
	if (in.isEmpty())
		return env->NewStringUTF("");
	return env->NewStringUTF(user_agent::desktop_form(in).toUtf8().constData());
}

extern "C" JNIEXPORT jstring JNICALL
Java_se_vibes_hydra_HydraWebView_injectedScripts(JNIEnv *env, jclass,
                                                               jlong id) {
	QString r;
	// Empty on timeout, which reads as "this view injects nothing" -- the same
	// answer a view with no scripts gives, and the safe one: a page that gets no
	// bridge script fails to find it, rather than the browser failing to draw.
	on_qt_thread([id] { return android_view::injected_scripts(id); }, &r);
	return env->NewStringUTF(r.toUtf8().constData());
}

std::function<void(const QUrl &)> android_view::s_external;

void android_view::set_external_handler(std::function<void(const QUrl &)> fn) {
	s_external = std::move(fn);
}

bool android_view::claims_external_url(const QString &url) {
	const QUrl u(url);
	if (u.isEmpty() || renders_as_page(u))
		return false;
	// Nothing to hand it to; let the WebView fail visibly rather than claim a
	// url and drop it.
	return s_external != nullptr;
}

void android_view::hand_to_external(const QString &url) {
	// Asked again rather than trusted: this runs later than the decision, on
	// another thread, and the handler could in principle have gone. Cheap, and
	// the alternative is calling through a null std::function.
	if (s_external && claims_external_url(url))
		s_external(QUrl(url));
}

bool android_view::allow_navigation(qint64 id, const QString &url,
                                     bool user_initiated) {
	android_view *v = s_views.value(id, nullptr);
	// **Allowed when nobody is listening**, in both senses: a view that has
	// gone away between the question being asked and reaching here, and a view
	// whose shell never set a decider. Refusing either would turn a missing
	// answer into a browser that will not browse.
	if (!v || !v->m_navigation_decider)
		return true;
	// Always the main frame: the Java side asks about nothing else, because a
	// subframe navigating itself is not the page going anywhere.
	return v->m_navigation_decider(QUrl(url), true, user_initiated);
}

void android_view::request_capture(qint64 id, const QString &origin,
                                    bool video, bool audio, qint64 token) {
	// One place that answers, however the question ends. Every early return
	// below goes through it, because a `PermissionRequest` that is never
	// answered leaves the page's promise pending for ever -- which looks to a
	// user exactly like a camera that is slow to start.
	// Behind the same switch the desktop's permission handler uses, and for the
	// same reason it gives: without it "we refused" and "we granted and the
	// engine could not deliver" look identical from the page's side. Off by
	// default because the origin is in the message, and logcat is not a private
	// place to put the address of every page that asks for a camera.
	const bool debug = qEnvironmentVariableIsSet("HYDRA_PERM_DEBUG");
	auto answer = [token, debug](bool granted) {
		if (debug)
			qWarning("capture: token %lld -> %s", (long long)token,
			          granted ? "granted" : "denied");
		QJniObject::callStaticMethod<void>(k_cls, "onCaptureDecision", "(JZ)V",
		                                    jlong(token), jboolean(granted));
	};
	if (debug)
		qWarning("capture: asked for %s%s by %s", video ? "video " : "",
		          audio ? "audio" : "", qPrintable(origin));

	android_view *v = s_views.value(id, nullptr);
	// **Refused when nobody is listening, which is the opposite of
	// `allow_navigation` above and deliberately so.** A missing navigation
	// decider must not stop the browser browsing; a missing permission decider
	// must not hand a page the camera. The safe direction is not a property of
	// the pattern, it is a property of what is being asked for.
	if (!v || !v->m_decider) {
		if (debug)
			qWarning("capture: no view (%d) or no decider (%d)", v == nullptr,
			          v && !v->m_decider);
		answer(false);
		return;
	}

	const QUrl o(origin);
	const permission_decider decider = v->m_decider;
	// Copied out with the decider, and for the same reason: the answer may
	// arrive after a dialog, by which time the view could be gone.
	const capability_note note = v->m_capability_note;

	// The operating system is a separate question from the site policy, with a
	// separate answer, and it is asked second. A user who denied this
	// application the camera has said something no per-site rule may override
	// -- but there is no point spending that dialog on a site the shield was
	// going to refuse anyway.
	//
	// Asked one after the other rather than together: each is a dialog, and two
	// at once is a stack of them in front of somebody trying to join a call.
	std::function<void()> os_stage = [answer, video, audio, note, o] {
		// **The operating system's answer, written down where a person can read
		// it.** The shield's answer is already recorded; without this one a
		// report cannot separate "Android refused" from "everything allowed and
		// the page is still saying it has no camera", which are different faults
		// with different owners.
		auto say = [note, o](policy::feature f, bool ok) {
			if (note)
				note(o, f, ok ? QStringLiteral("the system granted it")
				               : QStringLiteral("the system refused it"));
		};
		auto then_audio = [answer, audio, say](bool ok) {
			if (!ok)     { answer(false); return; }
			if (!audio)  { answer(true);  return; }
			qApp->requestPermission(QMicrophonePermission{}, qApp,
			                         [answer, say](const QPermission &p) {
				const bool got = p.status() == Qt::PermissionStatus::Granted;
				say(policy::feature::microphone, got);
				answer(got);
			});
		};
		if (!video) {
			then_audio(true);
			return;
		}
		qApp->requestPermission(QCameraPermission{}, qApp,
		                         [then_audio, say](const QPermission &p) {
			const bool got = p.status() == Qt::PermissionStatus::Granted;
			say(policy::feature::camera, got);
			then_audio(got);
		});
	};

	// **The policy stage is a chain now, not two `if`s.** It read
	// `if (video && !decider(o, camera))`, which was only possible while the
	// decider answered from a table; it can now put a dialog on the screen, and
	// a question that has to wait for a person cannot be asked inside a
	// condition.
	//
	// A call wanting both, on a site never asked about, can therefore cost four
	// prompts in a row: camera and microphone from the shield, then the same
	// two from Android. Only the first time -- Android remembers its own
	// answers, and the shield's prompt offers to. Worth it in exchange for the
	// two questions staying separate answers, which is what lets somebody grant
	// a meeting a microphone and not a camera.
	auto policy_audio = [decider, o, audio, answer, os_stage](bool ok) {
		if (!ok) { answer(false); return; }
		if (!audio) { os_stage(); return; }
		decider(o, policy::feature::microphone,
		         [answer, os_stage](bool granted) {
			if (!granted) { answer(false); return; }
			os_stage();
		});
	};
	if (!video) {
		policy_audio(true);
		return;
	}
	decider(o, policy::feature::camera, policy_audio);
}

void android_view::request_geolocation(qint64 id, const QString &origin,
                                        qint64 token) {
	const bool debug = qEnvironmentVariableIsSet("HYDRA_PERM_DEBUG");
	// One place that answers, however this ends. A geolocation callback that is
	// never invoked is not a refusal: the page waits, and its error handler
	// eventually reports a denial it was never actually given -- which is the
	// state this whole path was in before it existed.
	auto answer = [token, debug](bool granted) {
		if (debug)
			qWarning("geolocation: token %lld -> %s", (long long)token,
			          granted ? "granted" : "denied");
		QJniObject::callStaticMethod<void>(k_cls, "onGeolocationDecision", "(JZ)V",
		                                    jlong(token), jboolean(granted));
	};

	android_view *v = s_views.value(id, nullptr);
	// Refused when nobody is listening, as with capture and for the same reason:
	// a missing decider must not hand a page somebody's whereabouts.
	if (!v || !v->m_decider) {
		answer(false);
		return;
	}

	const QUrl o(origin);
	const capability_note note = v->m_capability_note;
	// The shield first, then Android. Asking the operating system for a
	// permission the shield was going to refuse spends a dialog on nothing.
	v->m_decider(o, policy::feature::geolocation,
	              [answer, debug, note, o](bool granted) {
		if (!granted) {
			answer(false);
			return;
		}
		// **Precise or nothing is not the ask.** `QLocationPermission` defaults
		// to approximate accuracy, and that is the right default for a browser:
		// a page wanting a city does not need a doorstep, and Android lets the
		// person downgrade a precise request anyway. A page that needs better
		// will say so, and this is the layer that would have to grow an option
		// for it rather than the one that assumes.
		qApp->requestPermission(QLocationPermission{}, qApp,
		                         [answer, debug, note, o](const QPermission &p) {
			const bool ok = p.status() == Qt::PermissionStatus::Granted;
			if (debug)
				qWarning("geolocation: the OS %s", ok ? "granted" : "refused");
			if (note)
				note(o, policy::feature::geolocation,
				      ok ? QStringLiteral("the system granted it")
				         : QStringLiteral("the system refused it"));
			answer(ok);
		});
	});
}

void android_view::choose_file(qint64 id, bool multiple, const QString &accept) {
	QStringList picked;
	{
		QFileDialog dialog(nullptr);
		dialog.setFileMode(multiple ? QFileDialog::ExistingFiles
		                             : QFileDialog::ExistingFile);
		dialog.setAcceptMode(QFileDialog::AcceptOpen);
		// `accept` is what the page put in its input: a comma-separated mix of
		// mime types and extensions. Only the mime types are passed on -- Qt's
		// Android dialog turns those into the picker's own filter -- and a list
		// with none is left unfiltered rather than filtered by a guess.
		QStringList mimes;
		for (const QString &one : accept.split(',', Qt::SkipEmptyParts)) {
			const QString t = one.trimmed();
			if (t.contains('/'))
				mimes << t;
		}
		if (!mimes.isEmpty())
			dialog.setMimeTypeFilters(mimes);
		if (dialog.exec() == QDialog::Accepted)
			picked = dialog.selectedFiles();
	}

	// Always answered, cancel included. An unanswered chooser callback is not a
	// no-op: the WebView keeps it, and every later file input on every later page
	// silently does nothing.
	QJniObject::callStaticMethod<void>(
	  k_cls, "deliverFiles", "(JLjava/lang/String;)V", jlong(id),
	  QJniObject::fromString(picked.join('\n')).object<jstring>());
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

namespace {

// The Permissions API, answered honestly, because Android's WebView will not
// answer it at all.
//
// **`navigator.permissions.query({name:"camera"})` is how a conferencing site
// decides whether to bother.** Teams asks it, gets something other than
// `granted`, and never calls `getUserMedia` -- so nothing reaches this
// browser's permission handler, the camera is never opened, and the message it
// shows names the mechanism exactly: *"To give access, select the site
// information icon in your browser's address bar and turn on your mic."* That
// icon is the browser's own permission state, and in a WebView it is not wired
// to anything.
//
// Every other layer was measured working first: `enumerateDevices` reports
// devices before and after a grant, the shield allows, Android grants, and a
// page that simply calls `getUserMedia` gets two tracks. The only broken link
// was the one a polite page checks before asking.
//
// **This reports what would actually happen, which is why it is a shim and not
// a lie.** Three states, combined from the two gates that really decide:
//
//   * the shield says block            -> "denied"
//   * the shield says ask              -> "prompt"
//   * the shield allows, OS granted    -> "granted"
//   * the shield allows, OS has not    -> "prompt", because asking raises
//                                          Android's dialog and might be refused
//
// Built per navigation rather than registered once, because the shield's answer
// is per site and a script injected at view creation would carry the answer for
// whatever page happened to be open first.
QString build_permissions_shim(android_view *v, const QUrl &origin) {
	if (!v || !v->peek() || origin.isEmpty())
		return QString();

	auto state_for = [&](policy::feature f, Qt::PermissionStatus os) {
		const policy::setting s = v->peek()(origin, f);
		if (s == policy::setting::block)
			return QStringLiteral("denied");
		if (s != policy::setting::allow)
			return QStringLiteral("prompt");
		return os == Qt::PermissionStatus::Granted ? QStringLiteral("granted")
		                                            : QStringLiteral("prompt");
	};
	// **Both halves of the answer, on request.** The shim reported `granted`
	// for the microphone and `prompt` for the camera on a handset whose OS had
	// granted both and whose shield allowed both -- which is impossible from
	// the code above, so one of the two inputs is not what it looks like. The
	// answer is a function of exactly two things; logging them is the whole
	// diagnosis, and guessing between them is not.
	const Qt::PermissionStatus cam_os = qApp->checkPermission(QCameraPermission{});
	const Qt::PermissionStatus mic_os = qApp->checkPermission(QMicrophonePermission{});
	const QString cam = state_for(policy::feature::camera, cam_os);
	const QString mic = state_for(policy::feature::microphone, mic_os);
	if (qEnvironmentVariableIsSet("HYDRA_PERM_DEBUG"))
		qWarning("hydra-perm: origin=%s camera shield=%d os=%d -> %s | "
		          "microphone shield=%d os=%d -> %s",
		          qPrintable(origin.toString()),
		          int(v->peek()(origin, policy::feature::camera)), int(cam_os),
		          qPrintable(cam),
		          int(v->peek()(origin, policy::feature::microphone)), int(mic_os),
		          qPrintable(mic));

	return permissions_shim::source(cam, mic);
}

}  // namespace

QString android_view::document_start_script(qint64 id, const QUrl &url) {
	android_view *v = s_views.value(id);
	return v ? build_permissions_shim(v, url) : QString();
}

extern "C" JNIEXPORT jstring JNICALL
Java_se_vibes_hydra_HydraWebView_documentStartScript(JNIEnv *env, jclass,
                                                                   jlong id,
                                                                   jstring url) {
	// **The destination, not the current page.** A document-start script is
	// registered before the load begins, so `v->url()` is still the page being
	// left -- and the shield's answer is per site. Registering the old page's
	// answer for the new page is the bug this whole path exists to fix, in a
	// subtler form.
	const char *raw = url ? env->GetStringUTFChars(url, nullptr) : nullptr;
	const QUrl dest = QUrl(QString::fromUtf8(raw ? raw : ""));
	if (raw)
		env->ReleaseStringUTFChars(url, raw);
	QString r;
	on_qt_thread([id, dest] { return android_view::document_start_script(id, dest); },
	              &r);
	return env->NewStringUTF(r.toUtf8().constData());
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
	// Last, so it overrides anything a page script may have captured first.
	out += build_permissions_shim(v, v->url());
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

bool android_view::any_view_open() {
	return !s_views.isEmpty();
}

bool android_view::allow_third_party_cookies(const QString &page_url) {
	// No filter yet means the first navigation is racing startup. Answering
	// "allowed" would hand a page the permissive case by accident; the shell's
	// own default for the feature is to block, so match it.
	if (!s_filter)
		return false;
	return s_filter->allow_cookie(QUrl(page_url).host(), true);
}

void android_view::report_url(qint64 id, const QString &url) {
	// Onto the Qt thread: this arrives on Android's UI thread.
	QMetaObject::invokeMethod(qApp, [id, url] {
		if (android_view *v = s_views.value(id))
			v->on_url_from_java(url);
	}, Qt::QueuedConnection);
}

void android_view::report_nav_state(qint64 id, bool back, bool forward) {
	// Onto the Qt thread, like report_url: this arrives on Android's UI thread.
	QMetaObject::invokeMethod(qApp, [id, back, forward] {
		if (android_view *v = s_views.value(id))
			v->on_nav_state_from_java(back, forward);
	}, Qt::QueuedConnection);
}

void android_view::on_nav_state_from_java(bool back, bool forward) {
	if (m_can_back == back && m_can_forward == forward)
		return;
	m_can_back    = back;
	m_can_forward = forward;
	// The seam already had the signal for this, wired to `update_navigation`.
	emit history_changed();
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
		// And on the application, for the one thing this arrangement gets wrong.
		//
		// The WebView is composited *above* Qt's surface, so a Qt dialog opened
		// over the page is invisible: the button depresses and nothing appears.
		// Found by tapping "Media (1)" on a phone and watching nothing happen.
		//
		// The first fix used QEvent::WindowBlocked, which Qt sends to a window
		// covered by a *modal* dialog. That was half of it, and the half that was
		// easy to believe: the downloads dialog is shown rather than exec'd, sends
		// no such event, and came up with the page drawn through the middle of it.
		// Counting visible dialogs instead needs no assumption about modality.
		qApp->installEventFilter(this);
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

namespace {

// Whether this is something Qt puts in front of the page.
//
// Written as one predicate rather than repeated at both use sites, because the
// two disagreeing is exactly how menus came to be missed: the filter's guard and
// its counting loop both said "dialog", and neither said "popup".
//
// Tooltips are included. They are small and rare, and a tooltip that renders
// underneath the page is the same defect in miniature -- there is no reason to
// leave one window type out and find it again from a phone.
bool covers_the_page(const QObject *o) {
	const auto *w = qobject_cast<const QWidget *>(o);
	if (!w)
		return false;
	if (qobject_cast<const QDialog *>(w))
		return true;
	const Qt::WindowType t = w->windowType();
	return t == Qt::Popup || t == Qt::ToolTip;
}

}  // namespace

bool android_view::eventFilter(QObject *o, QEvent *e) {
	// Anything Qt draws on top of the page, while it is up: the native view has
	// to be out of the way, because it is drawn over everything Qt renders.
	//
	// **This counted dialogs only, and menus are not dialogs.** A `QMenu` is a
	// `Qt::Popup` window, so the toolbar's menus and every combo-box drop-down
	// went uncounted -- they opened underneath the WebView and could not be seen
	// at all. Reported from the phone, and it is the same fault the dialog case
	// was written for, found again one window type along.
	if (m_native && (e->type() == QEvent::Show || e->type() == QEvent::Hide) &&
	    covers_the_page(o)) {
		int open = 0;
		for (QWidget *w : QApplication::topLevelWidgets())
			if (w->isVisible() && covers_the_page(w))
				++open;
		// The event arrives *before* the widget's visibility changes, so a dialog
		// being shown is not yet in that count and one being hidden still is.
		if (e->type() == QEvent::Show)
			++open;
		else
			--open;
		m_blocked = open > 0;
		sync_geometry();
	}
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
	// Never visible while a dialog is up, whatever the widget thinks: the widget
	// is perfectly visible, it is just underneath something.
	QJniObject::callStaticMethod<void>(
	  k_cls, "setVisible", "(JZ)V", jlong(m_id),
	  jboolean(m_widget->isVisible() && !m_blocked && !m_obscured));
}

void android_view::set_desktop_site(bool on) {
	if (m_desktop_site == on)
		return;
	m_desktop_site = on;
	if (m_native)
		QJniObject::callStaticMethod<void>(k_cls, "setDesktopSite", "(JZ)V",
		                                    jlong(m_id), jboolean(on));
}

void android_view::set_obscured(bool on) {
	if (m_obscured == on)
		return;
	m_obscured = on;
	sync_geometry();
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

// The whole struct in one JNI call rather than five setters. These arrive
// together from the shell's policy lookup, and each one would otherwise cost
// its own hop to Android's UI thread -- which is where a WebView's settings
// may be touched and nowhere else.
//
// Four of the five map onto a `WebSettings` call meaning what the desktop's
// `QWebEngineSettings` attribute means, and `scrollbars` onto the View's own
// scrollbar flags. `HydraWebView.applySettings` names each mapping beside the
// call that makes it, because a mapping recorded only here would drift from
// the code that implements it.
//
// **Posted, never waited on.** Nothing here needs an answer, so there is no
// reason to block the Qt thread on Android's UI thread -- the deadlock this
// file already met once.
void android_view::apply_settings(const view_settings &s) {
	if (!m_native)
		return;
	QJniObject::callStaticMethod<void>(
	  k_cls, "applySettings", "(JZZZZZ)V", jlong(m_id),
	  jboolean(s.javascript), jboolean(s.images), jboolean(s.autoplay),
	  jboolean(s.popups), jboolean(s.scrollbars));
}

void android_view::set_zoom_factor(double factor) {
	// **Not remembered when there is no WebView.** `zoom_factor()` is
	// documented to answer 1.0 from a backend that cannot scale, and a
	// placeholder QLabel cannot; storing it anyway would have the shell report
	// a zoom percentage for a page that is not on the screen.
	if (!m_native)
		return;
	m_zoom = factor;
	// Percent, because that is the unit Android's `setInitialScale` takes.
	// **Not simply `factor * 100` on the far side**, and `HydraWebView.
	// setZoomFactor` is where that is explained: Android's scale is in device
	// pixels, so a literal 100 draws a page at a third of its size on a 3x
	// phone.
	QJniObject::callStaticMethod<void>(k_cls, "setZoomFactor", "(JI)V",
	                                    jlong(m_id), jint(qRound(factor * 100)));
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

namespace {

// The clear in flight, and the caller waiting on it. One at a time: the
// settings dialog offers a single button and the kiosk controller clears at
// moments that cannot overlap, so a queue would be machinery for a case that
// does not arise. A second request while one is live is answered `refused`
// rather than silently dropped.
web_view_factory::clear_note g_clear_note;
web_view_factory::clear_report g_clear_report;
bool g_clearing = false;

// The same ten seconds the desktop backend allows itself, and for the same
// reason: a clear that never answers leaves the dialog saying "Clearing..."
// for ever, which is the silence this feature exists to remove wearing a
// progress message. Android needs it more, not less -- the answer comes back
// through a Java callback on the UI thread, and if the Activity has gone or
// the JNI call did not land there is nothing that would ever fire.
constexpr int k_clear_deadline_ms = 10000;

void finish_clear() {
	// Whichever of the callback and the deadline arrives second finds this
	// false and does nothing, so the caller is answered exactly once.
	if (!g_clearing)
		return;
	g_clearing = false;
	const web_view_factory::clear_report report = g_clear_report;
	web_view_factory::clear_note note;
	note.swap(g_clear_note);
	if (note)
		note(report);
}

}  // namespace

// Java answers `removeAllCookies` here, on the UI thread. The boolean is
// whether anything was removed, which is not a count -- so it decides between
// `done` and `unconfirmed` and leaves `cookies_removed` alone.
extern "C" JNIEXPORT void JNICALL
Java_se_vibes_hydra_HydraWebView_onCookiesCleared(JNIEnv *, jclass,
                                                                jboolean removed) {
	QMetaObject::invokeMethod(qApp, [removed] {
		g_clear_report.cookies = removed == JNI_TRUE
		                            ? web_view_factory::clear_state::done
		                            : web_view_factory::clear_state::unconfirmed;
		if (removed != JNI_TRUE)
			g_clear_report.notes
			  << QStringLiteral("Android reported no cookie was removed; there "
			                     "may have been none.");
		finish_clear();
	});
}

void android_factory::clear_browsing_data(const browsing_data &what,
                                            clear_note done) {
	clear_report report;

	if (g_clearing) {
		report.notes << QStringLiteral("Another clear is already running.");
		if (what.cookies)       report.cookies       = clear_state::refused;
		if (what.cache)         report.cache         = clear_state::refused;
		if (what.visited_links) report.visited_links = clear_state::refused;
		if (done)
			done(report);
		return;
	}

	// **Refused, not silently done.** Android has no visited-link store to
	// empty. `WebView.clearHistory()` is the back/forward list, which is the
	// shell's own data and is shown in the tab tree -- clearing that here would
	// delete something the user did not ask about while reporting success for
	// something that never happened.
	if (what.visited_links) {
		report.visited_links = clear_state::refused;
		report.notes << QStringLiteral(
		  "Android has no visited-link store; nothing was cleared for that.");
	}

	if (what.cache) {
		// No completion signal exists for this one, so it is `unconfirmed`
		// however well it went. Needs a live WebView: `clearCache` is a method
		// on the view, not on a manager.
		const bool have_view = android_view::any_view_open();
		if (have_view) {
			QJniObject::callStaticMethod<void>(k_cls, "clearCache", "()V");
			report.cache = clear_state::unconfirmed;
			report.notes << QStringLiteral(
			  "Android confirms nothing about clearing the cache.");
		} else {
			report.cache = clear_state::refused;
			report.notes << QStringLiteral(
			  "No web view is open, and Android clears the cache through one.");
		}
	}

	if (!what.cookies) {
		if (done)
			done(report);
		return;
	}

	// Site data rides with the cookies, and it is the one thing this backend
	// does that the desktop cannot: `WebStorage.deleteAllData()` empties
	// localStorage, IndexedDB and WebSQL for every origin, where Qt 6.8 wraps
	// no equivalent at all. Said out loud in the report rather than left as a
	// pleasant surprise, because the two backends otherwise claim to have done
	// the same thing.
	report.notes << QStringLiteral(
	  "Site data was cleared as well -- localStorage, IndexedDB and WebSQL. "
	  "The desktop build cannot do that.");

	g_clear_report = report;
	g_clear_note   = std::move(done);
	g_clearing     = true;
	QTimer::singleShot(k_clear_deadline_ms, qApp, [] {
		if (!g_clearing)
			return;
		// Not `done`: nothing came back, so nothing is known. Saying so is the
		// whole point of `unconfirmed` -- and leaving `g_clearing` set would
		// refuse every later clear as "one is already running", which is how a
		// missed callback turns into a button that never works again.
		g_clear_report.cookies = web_view_factory::clear_state::unconfirmed;
		g_clear_report.notes << QStringLiteral(
		  "Android did not answer within ten seconds; whether the cookies went "
		  "is unknown.");
		finish_clear();
	});
	QJniObject::callStaticMethod<void>(k_cls, "clearCookiesAndSiteData", "()V");
}
#endif   // Q_OS_ANDROID
