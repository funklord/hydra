#include "address_suggest.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace address_suggest {

QString bare(const QString &url) {
	QString u = url.trimmed().toLower();
	const int scheme = u.indexOf(QLatin1String("://"));
	if (scheme > 0 && scheme < 12)
		u = u.mid(scheme + 3);
	if (u.startsWith(QLatin1String("www.")))
		u = u.mid(4);
	if (u.endsWith(QLatin1Char('/')) && u.count(QLatin1Char('/')) == 1)
		u.chop(1);
	return u;
}

namespace {

// Every typed word starts some word of the title.
bool title_words(const QStringList &typed, const QString &title) {
	static const QRegularExpression split(QStringLiteral("[^\\w]+"),
	                                      QRegularExpression::UseUnicodePropertiesOption);
	const QStringList words = title.toLower().split(split, Qt::SkipEmptyParts);
	for (const QString &t : typed) {
		bool hit = false;
		for (const QString &w : words)
			if (w.startsWith(t)) { hit = true; break; }
		if (!hit)
			return false;
	}
	return true;
}

}  // namespace

QList<suggestion> suggest(const QString &typed, const QList<page> &pages, int limit) {
	const QString q = typed.trimmed().toLower();
	if (q.isEmpty() || limit <= 0)
		return {};
	// What was typed, as the bar would compare it to an address: a pasted
	// `https://www.` is not something to match against.
	const QString qbare = bare(q);
	const QStringList words = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);

	struct scored { suggestion s; QDateTime seen; };
	QHash<QString, int> at;   // bare address -> index in `found`
	QList<scored> found;
	for (const page &p : pages) {
		if (p.url.isEmpty() || p.url == QLatin1String("about:blank"))
			continue;
		const QString b = bare(p.url);
		int rank = 0;
		if (!qbare.isEmpty() && b.startsWith(qbare))
			rank = 3;
		else if (title_words(words, p.title))
			rank = 2;
		else if (!qbare.isEmpty() && b.contains(qbare))
			rank = 1;
		if (!rank)
			continue;
		// One address once: the best rank, then the most recent visit.
		const auto it = at.constFind(b);
		if (it != at.constEnd()) {
			scored &old = found[*it];
			if (rank > old.s.rank || (rank == old.s.rank && p.seen > old.seen)) {
				old.s = suggestion{ p.title, p.url, rank };
				old.seen = p.seen;
			}
			continue;
		}
		at.insert(b, int(found.size()));
		found.push_back(scored{ suggestion{ p.title, p.url, rank }, p.seen });
	}
	std::stable_sort(found.begin(), found.end(), [](const scored &a, const scored &b) {
		if (a.s.rank != b.s.rank)
			return a.s.rank > b.s.rank;
		return a.seen > b.seen;
	});
	QList<suggestion> out;
	for (int i = 0; i < found.size() && i < limit; ++i)
		out << found[i].s;
	return out;
}

}  // namespace address_suggest
