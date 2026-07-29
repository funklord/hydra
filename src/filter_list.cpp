// SPDX-License-Identifier: GPL-3.0-or-later
#include "filter_list.h"

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

dry_run filter_list::evaluate(const filter_rule &r, const QStringList &observed,
                               const QString &site_host) {
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
	if (!r.cosmetic) {
		for (const QString &url : observed)
			if (matches(r.text, url))
				out.would_block << url;
	}
	return out;
}

bool filter_list::contains(const QString &text) const {
	for (const filter_rule &r : m_rules)
		if (r.text == text)
			return true;
	return false;
}

void filter_list::add(const filter_rule &r) {
	if (!contains(r.text))
		m_rules.push_back(r);
}

bool filter_list::blocks(const QString &url, const QString &site_host) const {
	Q_UNUSED(site_host);
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
