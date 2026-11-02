#pragma once

#include <QHash>
#include <functional>
#include <QList>
#include <QMetaMethod>
#include <QObject>
#include <QString>
#include <QStringList>

class QObject;

// Calling a shell object from a page's javascript, by name.
//
// On the desktop QWebChannel does this: it walks a QObject's meta-object,
// publishes the invokable methods to the page, and marshals calls back. Android
// has `addJavascriptInterface`, which exposes a *Java* object and knows nothing
// about QObject -- so the marshalling has to be written, and this is it. It is
// shared and tested rather than sitting in the Android file because it is the
// piece where a mistake is a security hole rather than a bug (architecture doc
// sec 19.5).
//
// **Every argument that arrives here is hostile.** The page chooses the object
// name, the method name and the arguments, so:
//
//   * Only methods declared on the class itself are reachable. QObject's own
//     slots are excluded by index, which matters more than it sounds: `QObject`
//     publishes `deleteLater()`, and a page that could call it would delete the
//     shell's bridge out from under the browser. QWebChannel draws the same line
//     in the same place.
//   * Only signals' counterparts -- slots and Q_INVOKABLE methods -- are callable.
//     Signals are not, so a page cannot forge a report the shell believes.
//   * Argument and return types are converted explicitly, and a type that is not
//     on the list is refused by name rather than coerced. The bridges use bool,
//     int, double and QString; anything else is a new decision and should read
//     like one.
//
// Results are JSON, always with the same shape: `{"ok":true,"value":...}` or
// `{"ok":false,"error":"..."}`. A page cannot tell "no such method" from "that
// threw" -- it gets an error either way -- but the shell's log can.
class bridge_invoker {
public:
	// Registering the same name twice replaces the object, which is what happens
	// when a view is rebuilt for a new page.
	void add(const QString &name, QObject *object);
	void remove(const QString &name);

	// The method list for one object, as JSON, so the page-side shim can build
	// proxies without hard-coding them:
	//   {"ok":true,"value":{"methods":[{"name":"active_now","args":0}, ...]}}
	QString describe(const QString &name) const;

	// Call one method. `args_json` is a JSON array. Runs on the caller's thread:
	// on Android that is a binder thread, so the Android side hops to the Qt
	// thread before calling this rather than making every bridge thread-safe.
	QString invoke(const QString &name, const QString &method,
	                const QString &args_json) const;

	bool has(const QString &name) const { return m_objects.contains(name); }

	// Registration order is not kept -- the shim registers each name on the page
	// independently, so only the set matters.
	QStringList names() const { return QStringList(m_objects.keyBegin(), m_objects.keyEnd()); }

private:
	QHash<QString, QObject *> m_objects;
};

// **The other direction: a bridge object's signals, carried to the page.**
// A backend with no QWebChannel (Android) emulates the page side by calling
// methods; nothing heard a signal, so a script waiting on one -- the element
// picker's `pick_requested`, SponsorBlock's `answer` -- waited for ever.
//
// Every public signal a watched object declares is connected to a slot of
// this relay's own, which reads the arguments by the signal's types and hands
// the sink the object's name, the signal's name and the arguments as a JSON
// array (strings, booleans, numbers; anything else is null). What the sink
// does with them -- run them into the page -- is the backend's.
class bridge_signal_relay : public QObject {
public:
	using sink = std::function<void(const QString &name, const QString &signal,
	                                 const QString &args_json)>;
	explicit bridge_signal_relay(QObject *parent = nullptr) : QObject(parent) {}
	void set_sink(sink s) { m_sink = std::move(s); }
	void watch(const QString &name, QObject *object);
	int qt_metacall(QMetaObject::Call call, int id, void **argv) override;

private:
	struct watched {
		QString     name;
		QString     signal_name;
		QMetaMethod signal;
	};
	QList<watched> m_watched;
	sink           m_sink;
};
