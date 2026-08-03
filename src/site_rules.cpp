// SPDX-License-Identifier: GPL-3.0-or-later
#include "site_rules.h"

#include <QFile>
#include <QSettings>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>

site_rules site_rules::defaults() {
	site_rules r;
	auto builtin = [&](const char *kind, const char *value, const char *note) {
		site_rule c;
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

	// Scripts that exist to detect a blocker. Matched against a file *name*, and
	// deliberately specific: the message these drive tells someone to lower
	// their protection, so nothing may match merely on the word "ad".
	builtin("detector", "fuckadblock", "the one that broke a measured page");
	builtin("detector", "blockadblock", "its better-known sibling");
	builtin("detector", "adblock-detector", "");
	builtin("detector", "adblockdetector", "");
	builtin("detector", "detectadblock", "");
	builtin("detector", "antiblock", "");
	builtin("detector", "adbdetect", "");
	return r;
}

void site_rules::add(const site_rule &r) {
	// A learned rule that is generic is a candidate for the binary. Setting the
	// flag here rather than at the call sites means it cannot be forgotten by
	// whichever path learns the next one.
	site_rule c = r;
	// Generic *and ours*. A rule somebody sent is not something to propose for
	// everyone's binary on their say-so; it has been vouched for by nobody here.
	if (!c.builtin && !c.imported && c.generic())
		c.promote = true;
	m_rules.append(c);
}

site_rules site_rules::for_host(const QString &host) const {
	site_rules out;
	for (const site_rule &r : m_rules)
		if (r.generic() || r.host.compare(host, Qt::CaseInsensitive) == 0)
			out.m_rules.append(r);
	return out;
}

QList<site_rule> site_rules::promotable() const {
	QList<site_rule> out;
	for (const site_rule &r : m_rules)
		if (r.promote && !r.builtin && r.generic())
			out.append(r);
	return out;
}

namespace {

// Ordinary buttons that appear on ordinary pages. A consent pattern that fires
// on any of these is not a consent pattern; it is a licence to press whatever
// is in front of it. This is the same idea as §12.4's static breadth check on a
// filter rule — decide what a rule *would* do before letting it do anything —
// and the list is deliberately full of things that are destructive, expensive,
// or simply nothing to do with cookies.
const char *k_decoys[] = {
	"Delete account", "Delete", "Buy now", "Confirm order", "Pay", "Subscribe",
	"Unsubscribe", "Send", "Post", "Reply", "Log out", "Sign out", "Transfer",
	"Continue to checkout", "Yes", "No", "Cancel", "Save", "Submit", "Next",
};

}  // namespace

QString site_rules::why_unsafe(const site_rule &r) {
	static const QStringList kinds = { "container", "reject", "accept", "detector" };
	if (!kinds.contains(r.kind))
		return QString("unknown kind \"%1\"").arg(r.kind.left(20));
	const QString v = r.value.trimmed();
	if (v.isEmpty())
		return QStringLiteral("empty rule");
	if (v.size() > 200)
		return QStringLiteral("absurdly long");

	if (r.kind == "detector") {
		// A detector name drives a message telling someone to lower their
		// protection, and it matches as a substring of a file name. Two letters
		// would accuse half the web.
		if (v.size() < 5)
			return QStringLiteral("too short to identify a script");
		if (!QRegularExpression("^[A-Za-z0-9._-]+$").match(v).hasMatch())
			return QStringLiteral("a detector is a file-name fragment, not a "
			                       "pattern");
		return QString();
	}

	if (r.kind == "container") {
		// A selector, not a regex. `*` or `body` would hand every page's first
		// button to the clicker.
		if (v == "*" || v == "body" || v == "html" || v == "div")
			return QStringLiteral("matches the whole page");
		return QString();
	}

	// reject / accept: a regex that will be tested against button labels.
	QRegularExpression re(v, QRegularExpression::CaseInsensitiveOption);
	if (!re.isValid())
		return QStringLiteral("not a valid pattern");
	if (re.match(QString()).hasMatch())
		return QStringLiteral("matches an empty label, so it matches anything");
	for (const char *d : k_decoys) {
		const QString decoy = QString::fromUtf8(d);
		if (re.match(decoy).hasMatch())
			return QString("would also press \"%1\"").arg(decoy);
	}
	return QString();
}

QJsonObject site_rules::export_learned() const {
	QJsonArray arr;
	for (const site_rule &r : m_rules) {
		if (r.builtin)
			continue;
		QJsonObject o;
		o.insert("kind", r.kind);
		o.insert("value", r.value);
		if (!r.host.isEmpty()) o.insert("host", r.host);
		if (!r.note.isEmpty()) o.insert("note", r.note);
		arr.append(o);
	}
	QJsonObject root;
	root.insert("version", 1);
	root.insert("kind", "hydra-site-rules");
	root.insert("rules", arr);
	return root;
}

site_rules::import_result site_rules::judge_import(const QJsonObject &doc,
                                                     const QString &origin) {
	import_result out;
	if (doc.value("kind").toString() != "hydra-site-rules") {
		out.refused << "not a Hydra rule file";
		return out;
	}
	const QJsonArray arr = doc.value("rules").toArray();
	if (arr.isEmpty())
		out.refused << "the file contains no rules";
	for (const QJsonValue &v : arr) {
		const QJsonObject e = v.toObject();
		site_rule r;
		r.kind  = e.value("kind").toString();
		r.value = e.value("value").toString();
		r.host  = e.value("host").toString();
		r.note  = e.value("note").toString();
		// Never taken from the document. A sender does not get to declare that
		// their rule is a built-in, or that it is already flagged for shipping.
		r.builtin = false;
		r.promote = false;
		r.imported = true;
		// The caller's label, not the document's. A file that names itself
		// "Trusted community rules" is describing itself, which is worth exactly
		// nothing; where it actually came from is something only the importer
		// knows.
		r.origin = origin;
		const QString bad = why_unsafe(r);
		if (bad.isEmpty())
			out.accepted << r;
		else
			out.refused << QString("%1 (%2): %3").arg(r.kind, r.value.left(40), bad);
	}
	return out;
}

int site_rules::forget_imported() {
	int gone = 0;
	for (int i = m_rules.size() - 1; i >= 0; --i) {
		if (!m_rules[i].imported)
			continue;
		m_rules.removeAt(i);
		++gone;
	}
	return gone;
}

QJsonObject site_rules::to_json() const {
	QJsonArray arr;
	for (const site_rule &r : m_rules) {
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
		if (r.imported) o.insert("imported", true);
		if (!r.origin.isEmpty()) o.insert("origin", r.origin);
		arr.append(o);
	}
	QJsonObject root;
	root.insert("version", 1);
	root.insert("rules", arr);
	return root;
}

site_rules site_rules::from_json(const QJsonObject &o) {
	site_rules r = defaults();
	const QJsonArray arr = o.value("rules").toArray();
	for (const QJsonValue &v : arr) {
		const QJsonObject e = v.toObject();
		site_rule c;
		c.kind  = e.value("kind").toString();
		c.value = e.value("value").toString();
		c.host  = e.value("host").toString();
		c.note  = e.value("note").toString();
		c.imported = e.value("imported").toBool();
		c.origin   = e.value("origin").toString();
		if (c.kind.isEmpty() || c.value.isEmpty())
			continue;
		r.add(c);
	}
	return r;
}

bool site_rules::load_json(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
	if (!d.isObject())
		return false;
	*this = from_json(d.object());
	return true;
}

bool site_rules::load(const QString &path) {
	// INI first, then the JSON this file used to be. A rule set is small,
	// perishable and hand-editable by design -- the reason it is data rather
	// than code -- so it belongs in the format a person can repair. Reading the
	// old file once and writing the new one on the next save is what keeps that
	// change from costing anybody the rules they had.
	if (QFileInfo::exists(path)) {
		QSettings f(path, QSettings::IniFormat);
		if (f.status() == QSettings::NoError &&
		    f.value("hydra/kind").toString() == "siteRules") {
			// Built-ins are not in the file and must not be dropped by reading
			// one: start from the defaults and add what was stored, exactly as
			// the JSON path does through from_json.
			site_rules loaded = site_rules::defaults();
			const int n = f.beginReadArray("rules");
			for (int i = 0; i < n; ++i) {
				f.setArrayIndex(i);
				site_rule r;
				r.kind     = f.value("kind").toString();
				r.value    = f.value("value").toString();
				r.host     = f.value("host").toString();
				r.note     = f.value("note").toString();
				r.promote  = f.value("promote", false).toBool();
				r.imported = f.value("imported", false).toBool();
				r.origin   = f.value("origin").toString();
				if (!r.kind.isEmpty() && !r.value.isEmpty())
					loaded.add(r);
			}
			f.endArray();
			*this = loaded;
			return true;
		}
	}

	if (load_json(path))
		return true;
	QString legacy = path;
	if (legacy.endsWith(".ini")) {
		legacy.chop(4);
		legacy += ".json";
	}
	return legacy != path && load_json(legacy);
}

bool site_rules::save(const QString &path) const {
	QFile::remove(path);
	QSettings f(path, QSettings::IniFormat);
	f.setValue("hydra/format", 1);
	f.setValue("hydra/kind", "siteRules");

	f.beginWriteArray("rules");
	int n = 0;
	for (const site_rule &r : m_rules) {
		// Built-ins stay out, as they did in the JSON: they come from the binary,
		// and a copy in the file is a stale duplicate the day one changes.
		if (r.builtin)
			continue;
		f.setArrayIndex(n++);
		f.setValue("kind", r.kind);
		f.setValue("value", r.value);
		if (!r.host.isEmpty())   f.setValue("host", r.host);
		if (!r.note.isEmpty())   f.setValue("note", r.note);
		if (r.promote)           f.setValue("promote", true);
		if (r.imported)          f.setValue("imported", true);
		if (!r.origin.isEmpty()) f.setValue("origin", r.origin);
	}
	f.endArray();

	f.sync();
	return f.status() == QSettings::NoError;
}

QStringList site_rules::detectors() const {
	QStringList out;
	for (const site_rule &r : m_rules)
		if (r.kind == "detector")
			out << r.value;
	return out;
}

QString site_rules::to_script_literal() const {
	QJsonArray containers;
	QStringList reject, accept;
	for (const site_rule &r : m_rules) {
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
