#include "media_lookup.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
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

namespace {

const char *kind_word(media_kind k) {
	switch (k) {
	case media_kind::hls:  return "hls";
	case media_kind::dash: return "dash";
	default:               return "direct";
	}
}

media_kind kind_of(const QString &w) {
	if (w == QLatin1String("hls"))
		return media_kind::hls;
	if (w == QLatin1String("dash"))
		return media_kind::dash;
	return media_kind::direct;
}

QJsonObject to_json(const media_item &m) {
	QJsonObject headers;
	for (auto it = m.headers.cbegin(); it != m.headers.cend(); ++it)
		headers.insert(it.key(), it.value());
	return QJsonObject{
		{ "kind", kind_word(m.kind) },
		{ "url", m.url.toString() },
		{ "site", m.site_host },
		{ "label", m.label },
		{ "headers", headers },
		{ "file", m.file_name },
		{ "video", m.has_video },
		{ "audio", m.has_audio },
		{ "sound", m.audio.toString() },
		{ "sound_file", m.audio_file_name },
		{ "known", m.known_media },
		{ "chunk", double(m.chunk_size) },
		{ "sound_chunk", double(m.audio_chunk_size) },
		{ "chapters", m.chapters },
		{ "height", m.height },
		{ "rank", m.rank },
		{ "group", m.group },
		{ "lead", m.group_lead },
		{ "variant", m.variant },
	};
}

media_item from_json(const QJsonObject &o) {
	media_item m;
	m.kind      = kind_of(o.value("kind").toString());
	m.url       = QUrl(o.value("url").toString());
	m.site_host = o.value("site").toString();
	m.label     = o.value("label").toString();
	const QJsonObject headers = o.value("headers").toObject();
	for (auto it = headers.begin(); it != headers.end(); ++it)
		m.headers.insert(it.key(), it.value().toString());
	m.file_name        = o.value("file").toString();
	m.has_video        = o.value("video").toBool(true);
	m.has_audio        = o.value("audio").toBool(true);
	m.audio            = QUrl(o.value("sound").toString());
	m.audio_file_name  = o.value("sound_file").toString();
	m.known_media      = o.value("known").toBool();
	m.chunk_size       = qint64(o.value("chunk").toDouble());
	m.audio_chunk_size = qint64(o.value("sound_chunk").toDouble());
	m.chapters         = o.value("chapters").toString();
	m.height           = o.value("height").toInt();
	m.rank             = o.value("rank").toInt();
	m.group            = o.value("group").toString();
	m.group_lead       = o.value("lead").toBool();
	m.variant          = o.value("variant").toString();
	return m;
}

}  // namespace

bool media_lookups::save(const QString &path, const QDateTime &now) const {
	QJsonArray out;
	for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
		const media_lookup &e = it.value();
		if (e.st == media_lookup::state::running || e.expires <= now)
			continue;
		QJsonArray rows;
		for (const media_item &r : e.rows)
			rows.append(to_json(r));
		out.append(QJsonObject{
			{ "page", it.key() },
			{ "failed", e.st == media_lookup::state::failed },
			{ "expires", e.expires.toSecsSinceEpoch() },
			{ "note", e.note },
			{ "rows", rows },
		});
	}
	// Atomic, so a crash mid-write leaves the last good file rather than
	// half of one.
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
	return f.commit();
}

bool media_lookups::load(const QString &path, const QDateTime &now) {
	QFile f(path);
	if (!f.exists())
		return true;
	if (!f.open(QIODevice::ReadOnly))
		return false;
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isArray())
		return false;
	for (const QJsonValue &v : doc.array()) {
		const QJsonObject o = v.toObject();
		media_lookup e;
		e.st      = o.value("failed").toBool() ? media_lookup::state::failed
		                                       : media_lookup::state::done;
		e.expires = QDateTime::fromSecsSinceEpoch(qint64(o.value("expires").toDouble()),
		                                          QTimeZone::UTC);
		e.note    = o.value("note").toString();
		if (e.expires <= now)
			continue;
		for (const QJsonValue &r : o.value("rows").toArray())
			e.rows << from_json(r.toObject());
		m_entries.insert(o.value("page").toString(), e);
	}
	return true;
}
