#include "blocked_page.h"

#include <QUrlQuery>

namespace blocked_page {

namespace {

const char k_light[] = "--bg:#fbfaf8;--fg:#1c1b19;--muted:#5d5a55;--warn:#a1260d;"
                       "--code:#efece6";
const char k_dark[] = "--bg:#1b1a19;--fg:#e9e6e1;--muted:#a8a39b;--warn:#ff8a6b;"
                      "--code:#2a2826";
const char k_style[] =
  "body{margin:0;background:var(--bg);color:var(--fg);"
  "font:17px/1.55 system-ui,sans-serif}"
  "main{max-width:36em;margin:0 auto;padding:2.5em 1.25em}"
  "h1{color:var(--warn);font-size:1.5em;margin:0 0 .6em}"
  "p{margin:.8em 0}.muted{color:var(--muted)}"
  "code{background:var(--code);padding:.15em .35em;border-radius:3px;"
  "word-break:break-all}"
  "a.through{display:inline-block;margin-top:1.4em;color:var(--muted)}";

}  // namespace

QString build(const QUrl &page, const QString &rule, const QString &token, bool dark) {
	QUrl through = page;
	QUrlQuery q(through);
	q.addQueryItem(token_key(), token);
	through.setQuery(q);
	const QString host = page.host().toHtmlEscaped();
	QString out;
	out += QLatin1String("<!doctype html><html><head><meta charset=\"utf-8\">"
	                     "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src "
	                     "'none'; style-src 'unsafe-inline'\">"
	                     "<meta name=\"viewport\" content=\"width=device-width, "
	                     "initial-scale=1\"><meta name=\"color-scheme\" content=\"");
	out += dark ? QLatin1String("dark") : QLatin1String("light");
	out += QLatin1String("\"><title>Blocked: ") + host + QLatin1String("</title><style>:root{");
	out += QLatin1String(dark ? k_dark : k_light);
	out += QLatin1String("}") + QLatin1String(k_style);
	out += QLatin1String("</style></head><body><main><h1>Not opened: ") + host;
	out += QLatin1String("</h1><p>A filter list you subscribe to names this page as one "
	                     "not to visit. Lists do that for sites that pass themselves off as "
	                     "another to spread malware -- a fake download page, most often -- "
	                     "and for scams and trackers.</p><p class=\"muted\">The rule: <code>");
	out += rule.toHtmlEscaped();
	out += QLatin1String("</code></p><p class=\"muted\">The address: <code>");
	out += page.toString(QUrl::FullyEncoded).toHtmlEscaped();
	out += QLatin1String("</code></p><p>Back takes you to where you were.</p>"
	                     "<a class=\"through\" href=\"");
	out += through.toString(QUrl::FullyEncoded).toHtmlEscaped();
	out += QLatin1String("\">Open it anyway, for this session</a></main></body></html>");
	return out;
}

}  // namespace blocked_page
