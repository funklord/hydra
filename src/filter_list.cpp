// SPDX-License-Identifier: GPL-3.0-or-later
#include "filter_list.h"

#include <QReadLocker>
#include <QWriteLocker>

#include <QFile>
#include <QTextStream>
#include <QUrl>

namespace {

// Cosmetic selectors that would hide a whole page if applied generically.
const char *k_generic_tags[] = {
	"div", "span", "body", "html", "a", "img", "iframe", "p", "section",
	"main", "article", "*",
};

}  // namespace

bool filter_list::parse_rule(const QString &line, filter_rule *out) {
	const QString t = line.trimmed();
	if (t.isEmpty() || t.startsWith('!') || t.startsWith('['))
		return false;   // comment or list header

	filter_rule r;
	r.text = t;
	const int hash = t.indexOf("##");
	if (hash >= 0) {
		r.cosmetic = true;
		r.scope    = t.left(hash);
	} else {
		r.cosmetic = false;
		if (t.startsWith("||")) {
			const int end = t.indexOf('^');
			r.scope = (end > 2) ? t.mid(2, end - 2) : t.mid(2);
		}
	}
	*out = r;
	return true;
}

bool filter_list::matches(const QString &pattern, const QString &url) {
	QString p = pattern;
	// `||host^` — anchored at a domain boundary.
	if (p.startsWith("||")) {
		p.remove(0, 2);
		const int caret = p.indexOf('^');
		if (caret >= 0)
			p = p.left(caret);
		if (p.isEmpty())
			return false;
		const QString host = QUrl(url).host();
		return host == p || host.endsWith("." + p);
	}
	// Bare substring; `*` is the only wildcard worth supporting here.
	if (p.contains('*')) {
		const QStringList parts = p.split('*', Qt::SkipEmptyParts);
		int at = 0;
		for (const QString &part : parts) {
			at = url.indexOf(part, at);
			if (at < 0)
				return false;
			at += part.size();
		}
		return true;
	}
	return !p.isEmpty() && url.contains(p);
}

bool filter_list::cosmetic_matches(const QString &selector, const picked_element &e) {
	if (!e.is_valid())
		return false;
	// The rightmost compound is the one that describes the target element;
	// anything to its left constrains ancestors we cannot see from here.
	QString last = selector.trimmed();
	for (const QChar sep : {QChar(' '), QChar('>'), QChar('+'), QChar('~')}) {
		const int at = last.lastIndexOf(sep);
		if (at >= 0)
			last = last.mid(at + 1);
	}
	last = last.trimmed();
	if (last.isEmpty())
		return false;

	// Split "tag#id.a.b" into its parts, keeping the leading tag if present.
	QString tag, token;
	QStringList ids, classes;
	QChar kind = QChar(' ');
	auto flush = [&] {
		if (token.isEmpty())
			return;
		if (kind == QChar('#'))      ids << token;
		else if (kind == QChar('.')) classes << token;
		else                         tag = token.toLower();
		token.clear();
	};
	for (const QChar c : last) {
		if (c == QChar('#') || c == QChar('.')) { flush(); kind = c; continue; }
		if (c == QChar('[') || c == QChar(':')) break;   // attribute/pseudo: not checked
		token.append(c);
	}
	flush();

	if (!tag.isEmpty() && tag != "*" && tag != e.tag)
		return false;
	for (const QString &id : ids)
		if (id != e.id)
			return false;
	for (const QString &c : classes)
		if (!e.classes.contains(c))
			return false;
	return !tag.isEmpty() || !ids.isEmpty() || !classes.isEmpty();
}

dry_run filter_list::evaluate(const filter_rule &r, const QStringList &observed,
                               const QString &site_host,
                               const picked_element &picked) {
	dry_run out;

	// --- Static breadth check: reject dangerously broad rules (§12.4).
	if (r.cosmetic) {
		const QString selector = r.text.mid(r.text.indexOf("##") + 2).trimmed();
		if (r.scope.isEmpty()) {
			out.rejected = true;
			out.reason   = "Cosmetic rule with no domain would apply to every site.";
			return out;
		}
		for (const char *tag : k_generic_tags) {
			if (selector == QLatin1String(tag)) {
				out.rejected = true;
				out.reason   = QString("Selector \"%1\" hides a generic element.")
				                   .arg(selector);
				return out;
			}
		}
	} else {
		QString host = r.scope;
		if (host.isEmpty()) {
			out.rejected = true;
			out.reason   = "Network rule with no host would match everywhere.";
			return out;
		}
		// A rule that matches a whole TLD ("||com^") is never right.
		if (!host.contains('.')) {
			out.rejected = true;
			out.reason   = QString("\"%1\" matches an entire TLD.").arg(host);
			return out;
		}
		// Blocking the site's own origin would break the page it is meant to fix.
		if (!site_host.isEmpty() &&
		    (site_host == host || site_host.endsWith("." + host))) {
			out.rejected = true;
			out.reason   = QString("\"%1\" would block the page's own origin.")
			                   .arg(host);
			return out;
		}
	}

	// --- Simulation: show exactly what it would have blocked (§12.4).
	if (r.cosmetic && picked.is_valid()) {
		out.cosmetic_checked = true;
		out.cosmetic_hits =
			cosmetic_matches(r.text.mid(r.text.indexOf("##") + 2).trimmed(), picked);
	}
	if (!r.cosmetic) {
		for (const QString &url : observed)
			if (matches(r.text, url))
				out.would_block << url;
	}
	return out;
}

bool filter_list::remove(const QString &text) {
	QWriteLocker locker(&m_lock);
	for (int i = 0; i < m_rules.size(); ++i) {
		if (m_rules[i].text != text)
			continue;
		m_rules.removeAt(i);
		return true;
	}
	return false;
}

bool filter_list::contains(const QString &text) const {
	QReadLocker locker(&m_lock);
	return contains_locked(text);
}

// Same test, for callers that already hold the lock. Taking it twice is not a
// deadlock with QReadWriteLock's default non-recursive mode -- it is a hang.
bool filter_list::contains_locked(const QString &text) const {
	for (const filter_rule &r : m_rules)
		if (r.text == text)
			return true;
	return false;
}

void filter_list::add(const filter_rule &r) {
	QWriteLocker locker(&m_lock);
	if (!contains_locked(r.text))
		m_rules.push_back(r);
}

bool filter_list::blocks(const QString &url, const QString &site_host) const {
	// Read-locked: this is the one method called off the UI thread -- the
	// interceptor runs on Qt WebEngine's own thread -- while rules are added and
	// removed on the UI thread as the user accepts them. `rules()` hands out a
	// reference and stays UI-thread-only by contract; this does not.
	// `site_host` is unused, and that is a statement about the syntax rather than
	// an oversight. **A network rule here applies on every site.**
	//
	// This looked like a bug worth fixing -- `filter_rule::scope` is documented
	// as "domain for a site-specific rule" and blocks() ignored it -- and
	// filtering by it turned out to be exactly wrong. `parse_rule` fills `scope`
	// with two different things: for a cosmetic rule it is the site the rule
	// applies *on*, and for `||host^` it is the host being blocked. `evaluate()`
	// depends on the second meaning for its breadth check, which is why the field
	// reads as dual rather than confused. Comparing a blocked host against the
	// visiting site would have matched almost nothing and quietly disabled every
	// network rule -- a whole feature turned off by a change that reads as a fix.
	//
	// Per-site network rules would need `$domain=`, which this parser does not
	// implement. Until it does, the honest behaviour is the EasyList default:
	// network rules are global, and the per-site lever is the shield's ads
	// setting, which `request_filter` checks before ever calling this.
	Q_UNUSED(site_host);
	QReadLocker locker(&m_lock);
	for (const filter_rule &r : m_rules) {
		if (r.cosmetic)
			continue;   // cosmetic rules need DOM injection, not request blocking
		if (matches(r.text, url))
			return true;
	}
	return false;
}

bool filter_list::load(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;
	QWriteLocker locker(&m_lock);
	m_rules.clear();
	QTextStream in(&f);
	while (!in.atEnd()) {
		filter_rule r;
		if (parse_rule(in.readLine(), &r))
			m_rules.push_back(r);
	}
	return true;
}

bool filter_list::save(const QString &path) const {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return false;
	QTextStream out(&f);
	out << "! Hydra AI/user-authored filters — kept apart from imported\n"
	    << "! EasyList so upstream updates never clobber these (arch §12.5).\n";
	for (const filter_rule &r : m_rules) {
		if (!r.note.isEmpty())
			out << "! " << r.note << "\n";
		out << r.text << "\n";
	}
	return true;
}
