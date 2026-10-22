#include "media_lookup.h"

#include <QRegularExpression>
#include <QUrlQuery>

QString media_lookups::key(const QUrl &page) {
	return page.adjusted(QUrl::RemoveFragment).toString();
}

// The moment a signed address lapses, or 0. Two spellings in use: a query
// item (`...videoplayback?expire=1761150000&...`) and a path segment
// (`.../manifest/dash/expire/1761150000/...`).
static qint64 expires_at(const QUrl &url) {
	bool ok = false;
	const qint64 q = QUrlQuery(url).queryItemValue("expire").toLongLong(&ok);
	if (ok && q > 0)
		return q;
	static const QRegularExpression in_path(QStringLiteral("/expire/(\\d+)(/|$)"));
	const QRegularExpressionMatch m = in_path.match(url.path());
	return m.hasMatch() ? m.captured(1).toLongLong() : 0;
}

QDateTime media_lookups::expiry_for(const QList<media_item> &rows, const QDateTime &now) {
	qint64 first = 0;
	auto consider = [&first](const QUrl &u) {
		const qint64 at = u.isEmpty() ? 0 : expires_at(u);
		if (at > 0 && (first == 0 || at < first))
			first = at;
	};
	for (const media_item &r : rows) {
		consider(r.url);
		consider(r.audio);
	}
	if (first == 0)
		return now.addSecs(k_unsigned_minutes * 60);
	const QDateTime lapse = QDateTime::fromSecsSinceEpoch(first).addSecs(-k_margin_minutes * 60);
	// Never past the unsigned hour either: an address that claims a week is
	// still an answer about a page that may have changed.
	return std::min(lapse, now.addSecs(k_unsigned_minutes * 60));
}

const media_lookup *media_lookups::find(const QUrl &page, const QDateTime &now) {
	const auto it = m_entries.find(key(page));
	if (it == m_entries.end())
		return nullptr;
	if (it->st != media_lookup::state::running && it->expires <= now) {
		m_entries.erase(it);
		return nullptr;
	}
	return &*it;
}

void media_lookups::start(const QUrl &page) {
	m_entries.insert(key(page), media_lookup{});
}

void media_lookups::finish(const QUrl &page, const QList<media_item> &rows,
                           const QString &note, const QDateTime &now) {
	media_lookup e;
	e.st      = media_lookup::state::done;
	e.rows    = rows;
	e.note    = note;
	e.expires = expiry_for(rows, now);
	m_entries.insert(key(page), e);
}

void media_lookups::fail(const QUrl &page, const QString &error, const QDateTime &now) {
	media_lookup e;
	e.st      = media_lookup::state::failed;
	e.note    = error;
	e.expires = now.addSecs(k_failure_minutes * 60);
	m_entries.insert(key(page), e);
}

void media_lookups::forget(const QUrl &page) {
	m_entries.remove(key(page));
}

int media_lookups::clear() {
	const int n = int(m_entries.size());
	m_entries.clear();
	return n;
}
