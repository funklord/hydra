// What a request decision costs with the person's own subscribed lists, and
// nothing else running. Live only in that it reads ~/.local/share/Hydra.
//
//     ./test/build-make/try_filter_cost [rounds]
//
// **Why this exists**: the rules with options and the exceptions
// (`network_rules.h`) took a decision from about 80 to over 300 microseconds
// a request, measured 2026-10-29 inside `try_youtube_wall` -- where the page
// is still running and the figure is noisy. This times `decide` alone, with
// and without the rules with options, over addresses shaped like a page's,
// and is what to run under `perf record` to see where the time goes.
#include "filter_list.h"
#include "filter_subscription.h"
#include "network_rules.h"
#include "policy_engine.h"
#include "request_filter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <cstdio>

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);
	const int rounds = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 2000;
	const QString dir = QDir::homePath() + "/.local/share/Hydra/filters-subscribed";

	QList<filter_rule> plain;
	QList<network_rule> options;
	QSet<QString> cancelled;
	for (const QString &f : QDir(dir).entryList(QDir::Files)) {
		QFile body(dir + "/" + f);
		if (!body.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;
		const subscription_read rep =
		  filter_subscription::read(QString::fromUtf8(body.readAll()), 0, true);
		if (!rep.ok())
			continue;
		plain += rep.rules;
		options += rep.option_rules;
		for (const QString &c : rep.cancelled)
			cancelled.insert(c);
	}
	apply_badfilters(cancelled, &options, nullptr);
	std::printf("%lld plain rules, %lld with options\n", qlonglong(plain.size()),
	            qlonglong(options.size()));

	policy_engine policy;
	request_filter filter(&policy);
	filter_list subscribed;
	subscribed.replace(plain);
	filter.set_subscription_list(&subscribed);

	const QStringList urls = {
		"https://www.youtube.com/youtubei/v1/player?key=x",
		"https://i.ytimg.com/vi/abc/hqdefault.jpg",
		"https://fonts.gstatic.com/s/roboto/v30/x.woff2",
		"https://static.doubleclick.net/instream/ad_status.js",
		"https://cdn.example.com/assets/app.bundle.js?v=123",
		"https://www.google-analytics.com/analytics.js",
		"https://images.news.example/2026/10/photo-large.jpg",
		"https://api.example.org/v2/items?page=3&limit=50",
	};
	const auto time = [&](const char *what) {
		QElapsedTimer t;
		t.start();
		int blocked = 0;
		for (int i = 0; i < rounds; ++i)
			for (const QString &u : urls) {
				request_context ctx;
				ctx.url = QUrl(u);
				ctx.request_host = ctx.url.host();
				ctx.site_host = QStringLiteral("www.youtube.com");
				ctx.type = request_type::other;
				blocked += filter.decide(ctx).block ? 1 : 0;
			}
		std::printf("%-28s %6.1f us per request (%d blocked of %lld)\n", what,
		            double(t.nsecsElapsed()) / 1000.0 / (rounds * urls.size()),
		            blocked, qlonglong(rounds * urls.size()));
	};
	time("plain lists only:");
	filter.set_network_rules(options);
	time("with rules with options:");
	return 0;
}
