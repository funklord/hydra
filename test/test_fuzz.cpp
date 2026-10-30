// The parsers that read what the network sends, fed mutated input.
//
// Filter lists arrive from subscriptions, manifests and pages from any site,
// and each is read by a parser written here: a crash in one is a crash on a
// page somebody else wrote, and a regular expression that backtracks without
// bound is a page that hangs the browser. Neither shows in a suite of
// well-formed fixtures, which is all this tree had.
//
// **Deterministic, so a failure is a reproduction**: one fixed seed, and the
// iteration that failed is printed with its input. Mutations are the cheap
// ones that find parser faults -- truncation, a byte flipped, a span
// repeated or deleted, a token from the parser's own grammar spliced in.
// What is checked is that every input returns, under ASan and UBSan when the
// suite is built with `make test-sanitize`, and that none takes more than
// 200 ms, which no input of this size should.
//
//     HYDRA_FUZZ_SECONDS=60    run each parser that long instead of a moment
//     HYDRA_FUZZ_CORPUS=dir    add every file in dir as a seed, split into
//                              lines for the line parsers
#include "agent_fetch.h"
#include "dash_manifest.h"
#include "filter_list.h"
#include "filter_subscription.h"
#include "hls_playlist.h"
#include "network_rules.h"
#include "procedural_cosmetic.h"
#include "scriptlets.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QUrl>
#include <cstdio>
#include <functional>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

namespace {

QByteArray mutate(QByteArray s, QRandomGenerator &rng, const QList<QByteArray> &tokens) {
	const int steps = 1 + int(rng.bounded(4));
	for (int i = 0; i < steps; ++i) {
		const int n = s.size();
		const int at = n ? int(rng.bounded(n + 1)) : 0;
		switch (rng.bounded(7)) {
			case 0:  // truncate
				s.truncate(at);
				break;
			case 1:  // flip a byte
				if (n) s[int(rng.bounded(n))] = char(rng.bounded(256));
				break;
			case 2:  // delete a span
				if (n) s.remove(at, int(rng.bounded(qMax(1, n - at) + 1)));
				break;
			case 3: {  // repeat a span, up to a few times
				if (!n) break;
				const int len = 1 + int(rng.bounded(qMin(64, n)));
				const int from = int(rng.bounded(qMax(1, n - len + 1)));
				const QByteArray span = s.mid(from, len);
				s.insert(at, span.repeated(1 + int(rng.bounded(8))));
				break;
			}
			case 4:  // a token of the grammar
				if (!tokens.isEmpty())
					s.insert(at, tokens[int(rng.bounded(tokens.size()))]);
				break;
			case 5:  // a long run of one character: backtracking's favourite
				s.insert(at, QByteArray(int(rng.bounded(2000)), "(*\\[a,.|/^$"[rng.bounded(11)]));
				break;
			default:  // a byte inserted
				s.insert(at, char(rng.bounded(256)));
				break;
		}
		if (s.size() > 200000)
			s.truncate(200000);
	}
	return s;
}

struct target {
	const char *name;
	QList<QByteArray> seeds;
	QList<QByteArray> tokens;
	std::function<void(const QByteArray &)> run;
	bool lines = true;   // corpus files are split into lines for this one
};

}  // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const int seconds = qEnvironmentVariableIsSet("HYDRA_FUZZ_SECONDS")
	                      ? qMax(1, qEnvironmentVariableIntValue("HYDRA_FUZZ_SECONDS")) : 0;
	const int iterations = seconds ? INT_MAX : 400;
	const QString corpus = qEnvironmentVariable("HYDRA_FUZZ_CORPUS");

	const QUrl base("https://cdn.example/v/master.m3u8");
	QList<target> targets = {
		{ "filter_subscription::read",
			{ "! Title: x\n||ads.example^\nexample.com##.ad\n@@||good.example^$document\n"
				"example.com##+js(set-constant, a.b, false)\n"
				"example.com##div:has-text(/promo/i):upward(2)\n"
				"/banner[0-9]+/$image,domain=a.example|~b.a.example\n" },
			{ "##", "#@#", "#?#", "##+js(", "$", ",", "|", "||", "^", "~", "/", "\\", ":has(",
				":has-text(/", ":upward(", ":matches-css(", ":xpath(", "redirect=", "removeparam=",
				"domain=", "denyallow=", "to=", "csp=", "important", "badfilter", "@@", "\n", "!" },
			[](const QByteArray &b) { filter_subscription::read(QString::fromUtf8(b), 0, true); },
			false },
		{ "filter_subscription::classify",
			{ "||ads.example^$script,third-party", "example.com##.ad > div:nth-child(2)",
				"example.com##+js(json-prune, adPlacements playerAds)",
				"example.com##p:has-text(/^Sponsored$/):upward(div)",
				"/^https?:\\/\\/[a-z]+\\.example\\/ad/$xhr,to=example.com",
				"||x.example^$removeparam=/^utm_/", "@@||y.example^$generichide" },
			{ "##", "#?#", "+js(", "$", ",", "|", "^", "~", "/", "(", ")", "[", "]", "{", "}",
				":has-text(", ":upward(", ":remove()", ":style(", "redirect-rule=", "removeparam=",
				"domain=", "to=", "permissions=", "header=" },
			[](const QByteArray &b) {
				filter_rule r; QString why; scriptlet_call c; network_rule o; QString cancels;
				filter_subscription::classify(QString::fromUtf8(b), &r, &why, &c, &o, &cancels);
			} },
		{ "filter_list::parse_rule",
			{ "||ads.example^", "|https://a.example/x|", "example.com,~b.example##.ad",
				"/ads?\\d+/" },
			{ "|", "||", "^", "*", "##", "#@#", "~", ",", "/", "\\" },
			[](const QByteArray &b) { filter_rule r; filter_list::parse_rule(QString::fromUtf8(b), &r); } },
		{ "parse_network_rule",
			{ "||ads.example^$script,redirect=noopjs,domain=a.example",
				"/\\/ad[sv]\\//$image,important", "||t.example^$removeparam=/^(utm_|fbclid)/",
				"*$script,denyallow=x.example|y.example,domain=z.example" },
			{ "$", ",", "=", "|", "~", "/", "\\", "^", "*", "(", ")", "[", "]", "{2,}", "?", "+" },
			[](const QByteArray &b) {
				network_rule r; QString why, target;
				parse_network_rule(QString::fromUtf8(b), &r, &why, &target);
			} },
		{ "procedural_cosmetic::parse",
			{ "div:has-text(/promo/i)", "article:has(> .label:has-text(Ad)):upward(3)",
				"p:matches-css(position: fixed):remove()", "div:xpath(//div[@id=\"a\"])",
				".x:min-text-length(200):watch-attr(class)" },
			{ ":has(", ":has-text(", ":upward(", ":matches-css(", ":matches-attr(", ":xpath(",
				":remove()", ":style(", ":not(", ")", "(", "/", "\\", "[", "]", ">", "+", "~" },
			[](const QByteArray &b) {
				procedural_cosmetic::rule r; QString why;
				procedural_cosmetic::parse(QString::fromUtf8(b), &r, &why);
			} },
		{ "scriptlets::parse_call",
			{ "set-constant, a.b.c, false", "json-prune, adPlacements playerAds, , propsToMatch, x",
				"trusted-edit-inbound-object, JSON.stringify, 0, [?.a][?.b*=\"c\"]+={\"d\":1}",
				"xml-prune, 'xpath(//*[name()=\"Period\"][contains(@id\\,\"-ad-\")])', , /xp/",
				"m3u-prune, /#EXT-X-DISCONTINUITY\\n#EXTINF[^\\n]*\\n/gm, .m3u8",
				// Past the 32 argument flags an int holds: shifted by 32 and
				// undefined, until it was found by this harness under UBSan.
				"remove-node-text, script, /x/, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, "
				"p, q, r, s, t, u, v, w, x, y, z, 1, 2, 3, 4, 5, 6, 7, 8, 9" },
			{ ",", "'", "\"", "\\", "/", "(", ")", "[", "]", "{", "}", "?.", "*=", "+=", "xpath(" },
			[](const QByteArray &b) {
				scriptlet_call c; QString why;
				scriptlets::parse_call(QString::fromUtf8(b), &c, &why);
			} },
		{ "hls::parse",
			{ "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e\"\n"
				"low/index.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=2000000\nhigh/index.m3u8\n",
				"#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:7\n"
				"#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV=0x00000000000000000000000000000001\n"
				"#EXTINF:9.97,\nseg7.ts\n#EXT-X-BYTERANGE:1000@200\n#EXTINF:10,\nseg8.ts\n#EXT-X-ENDLIST\n" },
			{ "#EXTINF:", "#EXT-X-STREAM-INF:", "#EXT-X-KEY:", "#EXT-X-MAP:", "#EXT-X-BYTERANGE:",
				"METHOD=", "URI=\"", "\"", ",", "@", "\n", "BANDWIDTH=", "-1", "999999999999999" },
			[base](const QByteArray &b) { hls::parse(b, base); }, false },
		{ "dash::parse",
			{ "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" "
				"mediaPresentationDuration=\"PT1M\"><Period><AdaptationSet mimeType=\"video/mp4\">"
				"<Representation id=\"v1\" bandwidth=\"800000\" width=\"640\" height=\"360\">"
				"<BaseURL>v1/</BaseURL><SegmentTemplate media=\"$RepresentationID$/$Number%05d$.m4s\" "
				"initialization=\"$RepresentationID$/init.mp4\" duration=\"4\" timescale=\"1\" "
				"startNumber=\"1\"/></Representation></AdaptationSet></Period></MPD>" },
			{ "<", ">", "</", "\"", "$Number$", "$Time$", "%0", "d$", "<SegmentTimeline>",
				"<S t=\"0\" d=\"4\" r=\"-1\"/>", "duration=\"", "timescale=\"0\"", "PT", "H", "M", "S" },
			[base](const QByteArray &b) { dash::parse(b, base); }, false },
		{ "agent_fetch::read",
			{ "<!doctype html><html><head><script type=\"application/ld+json\">"
				"{\"@graph\":[{\"isAccessibleForFree\":false,\"hasPart\":{\"isAccessibleForFree\":false,"
				"\"cssSelector\":\".paywalled\"}}]}</script></head><body><p>A paragraph long enough "
				"to be counted as one of the article's.<div class=paywalled><p>More.</p></div></body>" },
			{ "<p>", "</p>", "<p class=x>", "<script>", "</script>", "<div>", "class=", "\"", "'",
				"{", "}", "[", "]", "isAccessibleForFree", "cssSelector", "application/ld+json" },
			[](const QByteArray &b) {
				agent_fetch::reading r = agent_fetch::read("fuzz", 200, b);
				r.body = b;
				agent_fetch::servable(r);
			}, false },
	};

	// A corpus of real input, when one is named: lists and manifests as they
	// were fetched.
	if (!corpus.isEmpty()) {
		const QDir dir(corpus);
		for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
			QFile f(fi.filePath());
			if (!f.open(QIODevice::ReadOnly))
				continue;
			const QByteArray all = f.read(4 * 1024 * 1024);
			for (target &t : targets) {
				// Whole-text parsers get it in pieces of about 8 KB, cut at a
				// line: a 2 MB list as one input measures its size, not the
				// parser, against the time bound.
				if (!t.lines) {
					for (int at = 0, n = 0; at < all.size() && n < 200; ++n) {
						int end = all.indexOf('\n', at + 8192);
						end = end < 0 ? all.size() : end + 1;
						t.seeds << all.mid(at, end - at);
						at = end + int(all.size() / 200);
						if (at < all.size())
							at = qMax(end, all.indexOf('\n', at) + 1);
					}
					continue;
				}
				const QList<QByteArray> lines = all.split('\n');
				for (int i = 0; i < lines.size(); i += qMax(1, int(lines.size() / 2000)))
					if (!lines[i].trimmed().isEmpty())
						t.seeds << lines[i];
			}
		}
	}

	for (const target &t : targets) {
		section(t.name);
		QRandomGenerator rng(0x68796472);   // "hydr"
		QElapsedTimer total;
		total.start();
		qint64 worst = 0;
		QByteArray worst_input;
		int done = 0;
		for (const QByteArray &seed : t.seeds)
			t.run(seed);
		for (int i = 0; i < iterations; ++i) {
			if (seconds && total.elapsed() > seconds * 1000LL)
				break;
			const QByteArray in = mutate(t.seeds[int(rng.bounded(t.seeds.size()))], rng, t.tokens);
			QElapsedTimer one;
			one.start();
			t.run(in);
			const qint64 ms = one.elapsed();
			if (ms > worst) {
				worst = ms;
				worst_input = in;
			}
			++done;
		}
		check(worst <= 200,
		      QString("%1 inputs, every one returned; the slowest took %2 ms%3")
		        .arg(done).arg(worst)
		        .arg(worst > 200 ? QString(" on: %1")
		                             .arg(QString::fromLatin1(worst_input.left(300).toPercentEncoding(" /:.,=#$|^~()[]{}<>\"'")))
		                         : QString()));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
