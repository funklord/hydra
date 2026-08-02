// SPDX-License-Identifier: GPL-3.0-or-later
#include "consent_rules.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

consent_rules consent_rules::defaults() {
	consent_rules r;
	auto builtin = [&](const char *kind, const char *value, const char *note) {
		consent_rule c;
		c.kind = QString::fromUtf8(kind);
		c.value = QString::fromUtf8(value);
		c.note = QString::fromUtf8(note);
		c.builtin = true;
		r.add(c);
	};

	// Least permissive first. "Reject" is preferred wherever a banner offers it;
	// accept is the last resort and is still better than a page nobody can read,
	// which is the state the option exists to end.
	builtin("reject",
	         "^(reject|decline|refuse|deny)( all)?( cookies)?$"
	         "|^(only |use )?(strictly )?necessary( cookies)?( only)?$"
	         "|^essential( cookies)?( only)?$|^continue without accepting",
	         "the wording reject buttons converge on");
	builtin("accept",
	         "^(accept|allow|agree|got it|ok|okay|i agree|understood|continue)"
	         "( all)?( cookies)?$",
	         "last resort, for banners whose only exit is acceptance");

	// A handful of vendor containers, kept short on purpose: each is a promise
	// to maintain a selector someone else controls. The generic pass is what
	// carries everything not listed here.
	builtin("container", "#onetrust-banner-sdk", "OneTrust");
	builtin("container", "#CybotCookiebotDialog", "Cookiebot");
	builtin("container", "#didomi-notice", "Didomi");
	builtin("container", "#usercentrics-root", "Usercentrics");
	builtin("container", ".qc-cmp2-container", "Quantcast");
	return r;
}

void consent_rules::add(const consent_rule &r) {
	// A learned rule that is generic is a candidate for the binary. Setting the
	// flag here rather than at the call sites means it cannot be forgotten by
	// whichever path learns the next one.
	consent_rule c = r;
	if (!c.builtin && c.generic())
		c.promote = true;
	m_rules.append(c);
}

consent_rules consent_rules::for_host(const QString &host) const {
	consent_rules out;
	for (const consent_rule &r : m_rules)
		if (r.generic() || r.host.compare(host, Qt::CaseInsensitive) == 0)
			out.m_rules.append(r);
	return out;
}

QList<consent_rule> consent_rules::promotable() const {
	QList<consent_rule> out;
	for (const consent_rule &r : m_rules)
		if (r.promote && !r.builtin && r.generic())
			out.append(r);
	return out;
}

QJsonObject consent_rules::to_json() const {
	QJsonArray arr;
	for (const consent_rule &r : m_rules) {
		// Built-ins are not written out. They come from the binary, and copying
		// them into the file would mean a stale duplicate the day one changes.
		if (r.builtin)
			continue;
		QJsonObject o;
		o.insert("kind", r.kind);
		o.insert("value", r.value);
		if (!r.host.isEmpty()) o.insert("host", r.host);
		if (!r.note.isEmpty()) o.insert("note", r.note);
		if (r.promote) o.insert("promote", true);
		arr.append(o);
	}
	QJsonObject root;
	root.insert("version", 1);
	root.insert("rules", arr);
	return root;
}

consent_rules consent_rules::from_json(const QJsonObject &o) {
	consent_rules r = defaults();
	const QJsonArray arr = o.value("rules").toArray();
	for (const QJsonValue &v : arr) {
		const QJsonObject e = v.toObject();
		consent_rule c;
		c.kind  = e.value("kind").toString();
		c.value = e.value("value").toString();
		c.host  = e.value("host").toString();
		c.note  = e.value("note").toString();
		if (c.kind.isEmpty() || c.value.isEmpty())
			continue;
		r.add(c);
	}
	return r;
}

bool consent_rules::load(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
	if (!d.isObject())
		return false;
	*this = from_json(d.object());
	return true;
}

bool consent_rules::save(const QString &path) const {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	return f.write(QJsonDocument(to_json()).toJson(QJsonDocument::Indented)) > 0;
}

QString consent_rules::to_script_literal() const {
	QJsonArray containers;
	QStringList reject, accept;
	for (const consent_rule &r : m_rules) {
		if (r.kind == "container")   containers.append(r.value);
		else if (r.kind == "reject") reject << r.value;
		else if (r.kind == "accept") accept << r.value;
	}
	QJsonObject o;
	o.insert("containers", containers);
	// Joined into one alternation each, so the script does no rule arithmetic of
	// its own — it applies what it was given and nothing else.
	o.insert("reject", reject.join('|'));
	o.insert("accept", accept.join('|'));
	return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
