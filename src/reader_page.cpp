#include "reader_page.h"

#include <QtGlobal>

namespace reader_page {

namespace {

QString escaped(const QString &s) {
	return s.toHtmlEscaped();
}

// The colours for each scheme, in one place. Contrast on the background,
// computed by the WCAG formula: text 16.2:1 light and 13.3:1 dark, the muted
// lines 6.6:1 and 6.8:1, links 7.0:1 and 8.5:1 -- all past the 4.5:1 that
// body text needs.
const char *k_light =
  "--bg:#fbfbf8;--fg:#1d1d1f;--muted:#5a5a5f;--link:#0b55a8;--rule:#ddd;--code:#f0f0ec;";
const char *k_dark =
  "--bg:#1b1b1d;--fg:#e2e2e0;--muted:#a3a3a8;--link:#8cb8ff;--rule:#3a3a3e;--code:#262629;";

const char *k_style =
  "html{background:var(--bg);color:var(--fg);}"
  "body{margin:0;padding:1.2em 1.1em 3em;font:1.1em/1.65 Georgia,'Noto Serif',"
  "'DejaVu Serif',serif;overflow-wrap:break-word;}"
  "main{max-width:38em;margin:0 auto;}"
  "header{border-bottom:1px solid var(--rule);margin-bottom:1.4em;padding-bottom:.8em;}"
  ".site{font:.85em/1.4 system-ui,sans-serif;color:var(--muted);margin:0 0 .4em;}"
  ".site a{color:var(--muted);}"
  "h1{font-size:1.75em;line-height:1.25;margin:.2em 0 .3em;}"
  ".byline{font:.85em/1.4 system-ui,sans-serif;color:var(--muted);margin:0;}"
  "h2,h3,h4,h5,h6{line-height:1.3;margin:1.4em 0 .5em;}"
  "a{color:var(--link);}"
  "img{max-width:100%;height:auto;display:block;margin:1em auto;}"
  "figure{margin:1.2em 0;}figcaption{font-size:.85em;color:var(--muted);}"
  "blockquote{margin:1em 0;padding:0 0 0 1em;border-left:3px solid var(--rule);"
  "color:var(--muted);}"
  "pre,code{font-family:ui-monospace,'DejaVu Sans Mono',monospace;font-size:.9em;"
  "background:var(--code);}"
  "pre{padding:.8em;overflow:auto;}code{padding:0 .2em;}pre code{padding:0;}"
  "table{border-collapse:collapse;display:block;overflow:auto;}"
  "td,th{border:1px solid var(--rule);padding:.3em .5em;}"
  "hr{border:0;border-top:1px solid var(--rule);}";

}  // namespace

QString build(const reader_extract::article &a, const QUrl &page, bool dark) {
	const QString title = a.title.isEmpty() ? page.host() : a.title;
	const QString site = a.site.isEmpty() ? page.host() : a.site;
	QString line = a.byline;
	if (a.words > 0) {
		// About 230 words a minute, the figure reading-time estimates use.
		const int minutes = qMax(1, (a.words + 115) / 230);
		const QString time = QString("%1 min read").arg(minutes);
		line = line.isEmpty() ? time : line + QLatin1String(" ") + QChar(0x00b7) + QLatin1String(" ") + time;
	}
	// **Concatenated, never `arg()`-ed.** The title and the article are the
	// page's text, and a second `arg()` pass would read a "%5" inside them as
	// a placeholder of its own.
	QString out;
	out.reserve(a.html.size() + 4096);
	out += QLatin1String("<!doctype html><html");
	if (!a.lang.isEmpty())
		out += QLatin1String(" lang=\"") + escaped(a.lang) + QLatin1Char('"');
	out += QLatin1String(" dir=\"");
	out += a.dir == QLatin1String("rtl") ? QLatin1String("rtl") : QLatin1String("ltr");
	out += QLatin1String("\"><head><meta charset=\"utf-8\">"
	                     // No script at all, and nothing fetched but pictures and media.
	                     "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src "
	                     "'none'; img-src http: https: data:; media-src http: https:; "
	                     "style-src 'unsafe-inline'\">"
	                     "<meta name=\"viewport\" content=\"width=device-width, "
	                     "initial-scale=1\"><meta name=\"color-scheme\" content=\"");
	out += dark ? QLatin1String("dark") : QLatin1String("light");
	out += QLatin1String("\"><title>") + escaped(title) + QLatin1String("</title><style>:root{");
	out += QLatin1String(dark ? k_dark : k_light);
	out += QLatin1String("}") + QLatin1String(k_style);
	out += QLatin1String("</style></head><body><main><header><p class=\"site\"><a href=\"");
	out += escaped(page.toString(QUrl::FullyEncoded)) + QLatin1String("\">") + escaped(site);
	out += QLatin1String("</a></p><h1>") + escaped(title) + QLatin1String("</h1>");
	if (!line.isEmpty())
		out += QLatin1String("<p class=\"byline\">") + escaped(line) + QLatin1String("</p>");
	out += QLatin1String("</header><article>") + a.html;
	out += QLatin1String("</article></main></body></html>");
	return out;
}

}  // namespace reader_page
