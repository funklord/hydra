#include "subscription_updater.h"

#include <QFile>

#include <QDateTime>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>

subscription_updater::subscription_updater(const QString &index_path,
                                            const QString &dir,
                                            QObject *parent)
  : QObject(parent), m_index(index_path), m_dir(dir) {
	// **Seeded only when there is no index file**, and the distinction from
	// an empty one is the whole of the care here. `load_index` answers with
	// an empty list for three different situations: no file, a file holding
	// no entries, and a file it refused as malformed -- and it refuses rather
	// than repairs precisely so that a damaged index does not lose
	// subscriptions. Seeding on an empty answer would undo that: a malformed
	// index would be silently replaced by the defaults, and somebody who had
	// removed every subscription would find them back on the next launch.
	//
	// Asking the filesystem instead keeps all three cases apart. The index is
	// written straight away, so this runs once and a later removal sticks.
	const QList<subscription> shipped = filter_subscription::default_subscriptions();
	if (!QFile::exists(m_index)) {
		m_subs = shipped;
		for (subscription &s : m_subs)
			s.file = filter_subscription::mint_cache_name(m_dir, s.name);
		filter_subscription::save_index(m_index, m_subs);
		QStringList all;
		for (const subscription &s : shipped)
			all << s.url.toString();
		write_offered(all);
		return;
	}
	m_subs = filter_subscription::load_index(m_index);
	// **Not to a profile whose index read back empty**, which is somebody who
	// removed every subscription or a file `load_index` refused as malformed.
	// The first has opted out; the second must stay on disk as it is, and
	// writing the index here would replace it. Found by the tests above this
	// one, which asked both questions before this code existed.
	if (m_subs.isEmpty())
		return;

	// **A list shipped later is offered to an existing profile once.** Seeding
	// runs only without an index, so a profile made before a list was added
	// would never see it -- which is how the holder's profile, two lists old,
	// missed the four added on 2026-10-09. Once rather than whenever absent,
	// so a list somebody removed stays removed: the urls ever offered are kept
	// beside the index, and a profile older than that record was offered the
	// original two at its seeding.
	QStringList offered = read_offered();
	if (offered.isEmpty())
		offered = QStringList{
		  QStringLiteral("https://easylist.to/easylist/easylist.txt"),
		  QStringLiteral("https://ublockorigin.github.io/uAssets/filters/"
		                 "filters.txt") };
	bool added = false;
	for (const subscription &d : shipped) {
		const QString url = d.url.toString();
		if (offered.contains(url))
			continue;
		offered << url;
		bool have = false;
		for (const subscription &s : m_subs)
			have = have || s.url == d.url;
		if (have)
			continue;
		subscription s = d;
		s.file = filter_subscription::mint_cache_name(m_dir, s.name);
		m_subs.push_back(s);
		added = true;
	}
	if (added)
		filter_subscription::save_index(m_index, m_subs);
	write_offered(offered);
}

// **A cached body that still names includes it could have followed is due
// now**, whatever its age: it was fetched before this build followed them,
// and is the head of a list rather than the list. The holder's
// `filters.txt` was cached that way the day before includes were followed,
// and without this would have stayed headless until its next scheduled
// fetch. An assembled body has no followable `!#include` line left, so this
// fires once per such list.
bool subscription_updater::cached_unassembled(const subscription &s) const {
	if (s.file.isEmpty())
		return false;
	QFile f(QDir(m_dir).filePath(s.file));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	return !filter_subscription::includes_in(QString::fromUtf8(f.readAll()))
	            .isEmpty();
}

// The record of shipped lists already offered, one url a line, beside the
// index. Missing is an empty answer, which the caller reads as "older than
// this record".
QStringList subscription_updater::read_offered() const {
	QFile f(m_index + QStringLiteral(".offered"));
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return QStringList();
	return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'),
	                                              Qt::SkipEmptyParts);
}

void subscription_updater::write_offered(const QStringList &urls) const {
	QSaveFile f(m_index + QStringLiteral(".offered"));
	if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
		f.write(urls.join(QLatin1Char('\n')).toUtf8() + '\n');
		f.commit();
	}
}

bool subscription_updater::set_subscriptions(const QList<subscription> &subs) {
	m_subs = subs;
	return filter_subscription::save_index(m_index, m_subs);
}

int subscription_updater::update(bool force, int max_age_hours) {
	if (busy())
		return 0;
	m_queue.clear();
	m_promoted = 0;
	m_refused  = 0;
	const QDateTime now = QDateTime::currentDateTime();
	for (int i = 0; i < m_subs.size(); ++i) {
		const subscription &s = m_subs.at(i);
		if (!s.enabled || s.url.isEmpty())
			continue;
		// **Due, or forced.** A browser that re-fetches every list on every
		// launch is doing it for nobody: these change over a day and the
		// cached copy is in force meanwhile. `force` is the Update now button,
		// which exists because somebody who has just subscribed should not
		// have to wait a day to find out whether the url was right.
		if (!force && s.fetched.isValid() &&
		    s.fetched.secsTo(now) < qint64(max_age_hours) * 3600 &&
		    !cached_unassembled(s))
			continue;
		m_queue.push_back(i);
	}
	if (m_queue.isEmpty())
		return 0;
	start_next();
	return m_queue.size() + 1;   // the one now in flight, plus the rest
}

void subscription_updater::start_next() {
	if (m_queue.isEmpty()) {
		m_at = -1;
		// Written once at the end of a pass rather than after each fetch: the
		// index is small, and a half-written one is the thing `save_index`
		// takes care to avoid.
		if (m_promoted > 0 || m_refused > 0)
			filter_subscription::save_index(m_index, m_subs);
		emit updated(m_promoted, m_refused);
		return;
	}
	const int which = m_queue.takeFirst();
	m_at = which;
	if (!m_net)
		m_net = new QNetworkAccessManager(this);

	const subscription &s = m_subs.at(which);
	emit note(QString("Fetching %1...").arg(s.name.isEmpty() ? s.url.host()
	                                                          : s.name));

	QNetworkReply *reply = get(s.url);
	connect(reply, &QNetworkReply::finished, this, [this, reply, which] {
		finish(reply, which);
	});
}

QNetworkReply *subscription_updater::get(const QUrl &url) {
	QNetworkRequest req(url);
	// **Bounded from the outside as well as the inside.** The size check below
	// needs bytes to arrive to fire at all; a server that accepts the
	// connection and then says nothing is stopped by this instead.
	req.setTransferTimeout(30000);
	req.setMaximumRedirectsAllowed(5);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                  QNetworkRequest::NoLessSafeRedirectPolicy);
	// Identified as what it is. A list server deciding to refuse an unknown
	// client is entitled to, and a request that hides what it is makes that
	// its own problem to diagnose.
	req.setHeader(QNetworkRequest::UserAgentHeader,
	               QStringLiteral("hydra/filter-subscriptions"));

	QNetworkReply *reply = m_net->get(req);
	connect(reply, &QNetworkReply::downloadProgress, reply,
	         [reply](qint64 got, qint64) {
		if (got > k_max_bytes)
			reply->abort();
	});
	return reply;
}

void subscription_updater::finish(QNetworkReply *reply, int which) {
	subscription &s = m_subs[which];
	const QByteArray body = reply->readAll();
	const int status =
	  reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QNetworkReply::NetworkError err = reply->error();
	const QString err_text = reply->errorString();
	reply->deleteLater();

	const auto refuse = [&](const QString &why) {
		s.note = why;
		++m_refused;
		emit note(QString("%1: %2").arg(s.name, why));
	};

	if (err != QNetworkReply::NoError) {
		// **The cache is untouched: a list that cannot be reached is not a
		// list that has changed**, which is the whole reason the body is
		// cached rather than held in memory.
		//
		// The status first where there is one, because Qt calls a 404 an
		// error and its words for one are "Error transferring <url>" -- which
		// tells a person nothing they did not know. A list answering 404 has
		// moved, and that is the one thing worth reading. Measured: the test
		// below asked for the status and got the transfer message.
		refuse(status > 0 ? QString("the server answered %1").arg(status)
		                   : err_text);
	} else if (status != 200) {
		// Not an error as far as Qt is concerned: a 204, or a 200-shaped 404
		// page, arrives as a successful reply with the wrong thing in it.
		refuse(QString("the server answered %1").arg(status));
	} else {
		// **The includes first**, so what is read and cached is the list
		// uBlock means rather than its head. The head waits here while they
		// are fetched one at a time; `promote` runs once they are in.
		m_includes = filter_subscription::includes_in(QString::fromUtf8(body));
		if (!m_includes.isEmpty()) {
			m_head = body;
			m_included.clear();
			fetch_include();
			return;
		}
		promote(which, body);
		return;
	}
	start_next();
}

void subscription_updater::fetch_include() {
	const subscription &s = m_subs.at(m_at);
	const QString name = m_includes.first();
	emit note(QString("Fetching %1 for %2...").arg(name, s.name));
	QNetworkReply *reply = get(s.url.resolved(QUrl(name)));
	connect(reply, &QNetworkReply::finished, this,
	         [this, reply] { finish_include(reply); });
}

void subscription_updater::finish_include(QNetworkReply *reply) {
	const int which = m_at;
	subscription &s = m_subs[which];
	const QString name = m_includes.takeFirst();
	const QByteArray body = reply->readAll();
	const int status =
	  reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	// **A part is held to the list's own first test**: a captive portal or an
	// error page answering for one part would otherwise be spliced into a
	// list that is otherwise fine and pass, because the head's rules carry
	// the read. Measured: the test server answers an unknown path with 200
	// and an echo, and that was accepted as a part.
	const bool page = body.trimmed().startsWith('<');
	const bool ok = reply->error() == QNetworkReply::NoError && status == 200 &&
	                !page;
	reply->deleteLater();
	if (!ok) {
		// **One missing part refuses the whole update**, and the copy in hand
		// stays: a list assembled with a hole in it is a fetch that was not
		// the list, the case every other refusal here exists for.
		s.note = QString("%1 could not be fetched (%2), so the copy in hand "
		                  "is kept").arg(name).arg(page ? QStringLiteral("a web page")
		                                           : status > 0
		                                           ? QString::number(status)
		                                           : QStringLiteral("no answer"));
		++m_refused;
		emit note(QString("%1: %2").arg(s.name, s.note));
		m_head.clear();
		m_includes.clear();
		m_included.clear();
		start_next();
		return;
	}
	m_included.insert(name, QString::fromUtf8(body));
	if (!m_includes.isEmpty()) {
		fetch_include();
		return;
	}
	const QByteArray whole = filter_subscription::assemble(
	  QString::fromUtf8(m_head), m_included).toUtf8();
	m_head.clear();
	m_included.clear();
	promote(which, whole);
}

// Read, cache and count a list that has arrived whole, then the next.
void subscription_updater::promote(int which, const QByteArray &body) {
	subscription &s = m_subs[which];
	const auto refuse = [&](const QString &why) {
		s.note = why;
		++m_refused;
		emit note(QString("%1: %2").arg(s.name, why));
	};
	const subscription_read rep = filter_subscription::read(
	  QString::fromUtf8(body), s.rules, s.trusted);
	if (!rep.ok()) {
		refuse(rep.refusal);
	} else if (!QDir().mkpath(m_dir)) {
		refuse(QStringLiteral("the cache directory could not be made"));
	} else {
		if (s.file.isEmpty())
			s.file = filter_subscription::mint_cache_name(m_dir, s.name);
		QSaveFile out(QDir(m_dir).filePath(s.file));
		if (!out.open(QIODevice::WriteOnly) ||
		    out.write(body) != body.size() || !out.commit()) {
			// **Fetched and not promoted, said plainly.** The rules could
			// be put into the live list from memory and would then vanish
			// at the next launch, with the index claiming a fetch that
			// left nothing behind. A subscription whose cache cannot be
			// written is a subscription that is not working, and saying so
			// is worth more than one session of blocking.
			refuse(QStringLiteral("fetched, but the copy could not be "
			                       "written"));
		} else {
			s.fetched = QDateTime::currentDateTime();
			s.rules   = rep.accepted;
			s.note    = rep.summary();
			++m_promoted;
			emit note(QString("%1: %2").arg(s.name, rep.summary()));
		}
	}
	start_next();
}
