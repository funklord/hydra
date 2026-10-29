#include "agent_fetch.h"

#include "user_agent.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSharedPointer>

namespace agent_fetch {

QList<agent> catalog() {
	const QByteArray chrome =
	  QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
	                 "(KHTML, like Gecko) Chrome/%1.0.0.0 Safari/537.36")
	    .arg(user_agent::k_claimed_chrome).toLatin1();
	return {
		{ "browser", "a desktop browser with no cookies", chrome, {} },
		{ "from-google", "the same browser, arriving from a Google result", chrome,
		  "https://www.google.com/" },
		{ "googlebot", "Google's search crawler",
		  "Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)", {} },
		{ "bingbot", "Bing's search crawler",
		  "Mozilla/5.0 (compatible; bingbot/2.0; +http://www.bing.com/bingbot.htm)", {} },
		{ "facebook", "Facebook's link preview",
		  "facebookexternalhit/1.1 (+http://www.facebook.com/externalhit_uatext.php)", {} },
		{ "twitterbot", "X's link preview", "Twitterbot/1.0", {} },
	};
}

namespace {

QString plain(QString html) {
	static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
	static const QRegularExpression space(QStringLiteral("\\s+"));
	html.remove(tag);
	html.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
	html.replace(QLatin1String("&amp;"), QLatin1String("&"));
	return html.replace(space, QStringLiteral(" ")).trimmed();
}

// The JSON-LD's word on the paywall, from any object in any block -- a
// `@graph`, an array, or a bare object.
void paywall_of(const QJsonValue &v, reading *r, int depth = 0) {
	if (depth > 6)
		return;
	if (v.isArray()) {
		for (const QJsonValue &e : v.toArray())
			paywall_of(e, r, depth + 1);
		return;
	}
	if (!v.isObject())
		return;
	const QJsonObject o = v.toObject();
	const QJsonValue free = o.value("isAccessibleForFree");
	if ((free.isBool() && !free.toBool()) ||
	    free.toString().compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
		r->declared_paywall = true;
	const QString sel = o.value("cssSelector").toString();
	if (!sel.isEmpty() && r->paywalled_selector.isEmpty() &&
	    (free.isBool() || !free.toString().isEmpty()))
		r->paywalled_selector = sel;
	for (const QString &k : { QStringLiteral("hasPart"), QStringLiteral("@graph") })
		if (o.contains(k))
			paywall_of(o.value(k), r, depth + 1);
}

}  // namespace

reading read(const QString &agent, int status, const QByteArray &body) {
	reading r;
	r.agent = agent;
	r.status = status;
	r.bytes = body.size();
	const QString html = QString::fromUtf8(body);

	static const QRegularExpression para(
	  QStringLiteral("<p(?:\\s[^>]*)?>(.*?)</p>"),
	  QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
	for (auto it = para.globalMatch(html); it.hasNext();) {
		const QString t = plain(it.next().captured(1));
		if (t.size() < 40)
			continue;
		if (r.opening.isEmpty())
			r.opening = t.left(120);
		++r.paragraphs;
		r.text_chars += t.size();
	}

	static const QRegularExpression ld(
	  QStringLiteral("<script[^>]*application/ld\\+json[^>]*>(.*?)</script>"),
	  QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
	for (auto it = ld.globalMatch(html); it.hasNext();) {
		const QJsonDocument d = QJsonDocument::fromJson(it.next().captured(1).toUtf8());
		paywall_of(d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object()), &r);
	}
	// Whether the part said to be paywalled was sent: a class selector's
	// class on some element. Other selector shapes are not judged.
	static const QRegularExpression cls(QStringLiteral("^\\.([A-Za-z0-9_-]+)$"));
	const QRegularExpressionMatch m = cls.match(r.paywalled_selector.trimmed());
	if (m.hasMatch()) {
		const QRegularExpression on(
		  QStringLiteral("class\\s*=\\s*[\"'][^\"']*(?<![\\w-])%1(?![\\w-])")
		    .arg(QRegularExpression::escape(m.captured(1))));
		r.paywalled_present = on.match(html).hasMatch();
	}

	// **A bot check rather than the page**: one of the known challenge
	// pages, or a small answer with no paragraphs that is all script. Status
	// codes alone do not say it -- the one met answered 247.
	static const QRegularExpression known(
	  QStringLiteral("cf-chl|challenge-platform|Just a moment\\.\\.\\.|rbzns|"
	                 "captcha-delivery|_Incapsula_|px-captcha|"
	                 "Attention Required"),
	  QRegularExpression::CaseInsensitiveOption);
	r.challenge = known.match(html).hasMatch() ||
	              (r.paragraphs == 0 && r.bytes < 4096 &&
	               html.contains(QLatin1String("<script"), Qt::CaseInsensitive)) ||
	              status == 429;
	return r;
}

QString describe(const reading &r) {
	if (r.status == 0)
		return QString("%1: no answer (%2)").arg(r.agent, r.error);
	QString s = QString("%1: HTTP %2, %3 bytes").arg(r.agent).arg(r.status).arg(r.bytes);
	if (!r.error.isEmpty())
		return s + ", " + r.error;
	if (r.challenge)
		return s + ", a bot check rather than the page";
	s += QString(", %1 paragraphs, %2 characters of text").arg(r.paragraphs).arg(r.text_chars);
	if (r.declared_paywall) {
		s += ", declares itself not free";
		if (!r.paywalled_selector.isEmpty())
			s += QString(" (the rest under %1, %2)")
			       .arg(r.paywalled_selector,
			            r.paywalled_present ? QStringLiteral("which IS in this answer")
			                                : QStringLiteral("which is not in this answer"));
	}
	if (!r.opening.isEmpty())
		s += QString("; opens \"%1\"").arg(r.opening.left(80));
	return s;
}

}  // namespace agent_fetch

void agent_fetcher::fetch_all(
  const QUrl &url, std::function<void(const QList<agent_fetch::reading> &)> done) {
	const QList<agent_fetch::agent> agents = agent_fetch::catalog();
	if (m_busy || !url.isValid() ||
	    (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
		agent_fetch::reading r;
		r.agent = QStringLiteral("all");
		r.error = m_busy ? QStringLiteral("a fetch is already running")
		                 : QStringLiteral("not a web address");
		done({ r });
		return;
	}
	m_busy = true;
	// A fresh manager, and with it an empty cookie jar, every run.
	if (m_net)
		m_net->deleteLater();
	m_net = new QNetworkAccessManager(this);
	m_net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

	struct state {
		QList<agent_fetch::reading> readings;
		int left = 0;
		std::function<void(const QList<agent_fetch::reading> &)> done;
	};
	auto st = QSharedPointer<state>::create();
	st->readings.resize(agents.size());
	st->left = agents.size();
	st->done = std::move(done);

	for (int i = 0; i < agents.size(); ++i) {
		QNetworkRequest req(url);
		req.setHeader(QNetworkRequest::UserAgentHeader, agents[i].user_agent);
		if (!agents[i].referer.isEmpty())
			req.setRawHeader("Referer", agents[i].referer);
		req.setRawHeader("Accept", "text/html,application/xhtml+xml;q=0.9,*/*;q=0.8");
		req.setMaximumRedirectsAllowed(10);
		req.setTransferTimeout(k_timeout_ms);
		QNetworkReply *reply = m_net->get(req);
		const QString name = agents[i].name;
		connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64) {
			if (got > k_max_bytes)
				reply->abort();
		});
		connect(reply, &QNetworkReply::finished, this, [this, reply, st, i, name] {
			reply->deleteLater();
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			agent_fetch::reading r;
			if (status == 0) {
				r.agent = name;
				r.error = reply->errorString();
			} else {
				r = agent_fetch::read(name, status, reply->readAll().left(k_max_bytes));
				// A redirect still standing was refused, not empty: from
				// https to plain http, or past the limit.
				// The header, not RedirectionTargetAttribute: Qt leaves that
				// unset on a redirect its policy refused.
				const QUrl to = QUrl(QString::fromUtf8(reply->rawHeader("Location")));
				if (status >= 300 && status < 400 && to.isValid())
					r.error = QString("redirects to %1, not followed%2")
					            .arg(reply->url().resolved(to).toString(),
					                 reply->url().resolved(to).scheme() == QLatin1String("http")
					                   ? QStringLiteral(": it leaves https") : QString());
			}
			st->readings[i] = r;
			if (--st->left == 0) {
				m_busy = false;
				st->done(st->readings);
			}
		});
	}
}
