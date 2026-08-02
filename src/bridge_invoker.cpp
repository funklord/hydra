// SPDX-License-Identifier: GPL-3.0-or-later
#include "bridge_invoker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QVariant>

namespace {

QString fail(const QString &why) {
	QJsonObject o;
	o.insert("ok", false);
	o.insert("error", why);
	return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString ok(const QJsonValue &value) {
	QJsonObject o;
	o.insert("ok", true);
	o.insert("value", value);
	return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Methods a page may call: declared on the class itself, and callable rather
// than emittable. The index floor is the whole of the QObject-slot exclusion —
// see the header for why that one matters.
bool reachable(const QMetaObject *mo, int index) {
	if (index < QObject::staticMetaObject.methodCount())
		return false;
	const QMetaMethod m = mo->method(index);
	return m.access() == QMetaMethod::Public &&
	       (m.methodType() == QMetaMethod::Slot ||
	        m.methodType() == QMetaMethod::Method);
}

// The types a bridge argument may have. Refusing by name beats coercing: a
// silent conversion here would be the page choosing what the shell believes.
bool convert(const QJsonValue &in, QMetaType type, QVariant &out) {
	switch (type.id()) {
		case QMetaType::QString:
			if (!in.isString())
				return false;
			out = in.toString();
			return true;
		case QMetaType::Bool:
			if (!in.isBool())
				return false;
			out = in.toBool();
			return true;
		case QMetaType::Int:
			if (!in.isDouble())
				return false;
			out = in.toInt();
			return true;
		case QMetaType::Double:
			if (!in.isDouble())
				return false;
			out = in.toDouble();
			return true;
		default:
			return false;
	}
}

}  // namespace

void bridge_invoker::add(const QString &name, QObject *object) {
	if (name.isEmpty() || !object)
		return;
	m_objects.insert(name, object);
}

void bridge_invoker::remove(const QString &name) {
	m_objects.remove(name);
}

QString bridge_invoker::describe(const QString &name) const {
	QObject *obj = m_objects.value(name);
	if (!obj)
		return fail(QStringLiteral("no such bridge"));

	const QMetaObject *mo = obj->metaObject();
	QJsonArray methods;
	for (int i = 0; i < mo->methodCount(); ++i) {
		if (!reachable(mo, i))
			continue;
		const QMetaMethod m = mo->method(i);
		QJsonObject one;
		one.insert("name", QString::fromUtf8(m.name()));
		one.insert("args", m.parameterCount());
		methods.append(one);
	}
	QJsonObject value;
	value.insert("methods", methods);
	return ok(value);
}

QString bridge_invoker::invoke(const QString &name, const QString &method,
                                const QString &args_json) const {
	QObject *obj = m_objects.value(name);
	if (!obj)
		return fail(QStringLiteral("no such bridge"));

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(args_json.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isArray())
		return fail(QStringLiteral("arguments are not a JSON array"));
	const QJsonArray args = doc.array();

	// Find the method by name *and* arity. Overloads exist, and picking the
	// first name match would call whichever one the moc happened to emit first.
	const QMetaObject *mo = obj->metaObject();
	QMetaMethod found;
	for (int i = 0; i < mo->methodCount(); ++i) {
		if (!reachable(mo, i))
			continue;
		const QMetaMethod m = mo->method(i);
		if (QString::fromUtf8(m.name()) == method &&
		    m.parameterCount() == args.size()) {
			found = m;
			break;
		}
	}
	if (!found.isValid())
		return fail(QStringLiteral("no such method"));

	// Qt's generic invoke wants pointers that outlive the call, so the converted
	// values are held here rather than in the loop. Six is where the real bridges
	// top out -- mse_tap::report takes six -- and a seventh is one more line in
	// the switch below rather than a redesign.
	static const int k_max_args = 6;
	if (found.parameterCount() > k_max_args)
		return fail(QStringLiteral("too many arguments"));
	QVariant       held[k_max_args];
	QGenericArgument generic[k_max_args];
	for (int i = 0; i < found.parameterCount(); ++i) {
		if (!convert(args.at(i), found.parameterMetaType(i), held[i]))
			return fail(QStringLiteral("argument %1 is not a %2")
			                .arg(i)
			                .arg(QString::fromUtf8(found.parameterMetaType(i).name())));
		generic[i] = QGenericArgument(held[i].typeName(), held[i].constData());
	}

	// One overload family for every call, chosen deliberately.
	//
	// QMetaMethod::invoke comes in two: a templated one taking real values, and
	// the older one taking QGenericArgument. **They cannot be mixed.** Passing
	// QGenericArgument parameters together with a Q_RETURN_ARG return fails with
	// "cannot convert formal parameter 0", which reads like an argument bug and
	// is really overload resolution picking the templated form and then finding
	// a QGenericArgument where it wanted a value.
	//
	// Arguments are only known at runtime here, so the QGenericArgument form is
	// the one that fits, and the return argument is built by type name to match.
	// Trailing empty arguments are fine for it, which is why this is not
	// arity-switched.
	//
	// That form is declared under `QT_VERSION <= QT_VERSION_CHECK(7, 0, 0)`, so
	// Qt 7 removes it and this function has to be rewritten against the
	// templated one — which, taking values rather than type-erased pointers, will
	// mean dispatching on arity and type rather than looping. Worth knowing
	// before it is a build error.
	const QMetaType ret = found.returnMetaType();
	bool    bv = false;
	int     iv = 0;
	double  dv = 0;
	QString sv;
	void   *slot = nullptr;
	switch (ret.id()) {
		case QMetaType::Void:                        break;
		case QMetaType::Bool:    slot = &bv;         break;
		case QMetaType::Int:     slot = &iv;         break;
		case QMetaType::Double:  slot = &dv;         break;
		case QMetaType::QString: slot = &sv;         break;
		default:
			// Deliberately not coerced. A bridge that wants to return something
			// else is a decision to make on purpose, in this switch.
			return fail(QStringLiteral("return type %1 is not bridged")
			                .arg(QString::fromUtf8(ret.name())));
	}

	const QGenericReturnArgument rr =
		slot ? QGenericReturnArgument(ret.name(), slot) : QGenericReturnArgument();
	if (!found.invoke(obj, Qt::DirectConnection, rr, generic[0], generic[1],
	                  generic[2], generic[3], generic[4], generic[5]))
		return fail(QStringLiteral("invoke failed"));

	switch (ret.id()) {
		case QMetaType::Bool:    return ok(bv);
		case QMetaType::Int:     return ok(iv);
		case QMetaType::Double:  return ok(dv);
		case QMetaType::QString: return ok(sv);
		default:                 return ok(QJsonValue());
	}
}
