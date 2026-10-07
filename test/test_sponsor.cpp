// Sponsor segments: what a url names, what goes out, what comes back, and
// where to jump.
//
// **The case that matters most is the filter in `parse`.** The service is
// asked about a hash *prefix* and answers with every video sharing it, so the
// answer contains other people's videos. Picking out the one being watched is
// what makes the query honest, and a bug there would skip at times belonging
// to a different video -- wrong in a way that looks like bad data rather than
// like a bug here.
#include "sponsor_segments.h"
#include "sponsor_skip.h"

#include "echo_server.h"
#include "policy_engine.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJSEngine>
#include <QJSValue>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static void spin(int ms) {
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("the id a url names, in every form a player arrives as");
	{
		struct one { const char *url; const char *want; const char *what; };
		const QList<one> cases = {
			{ "https://www.youtube.com/watch?v=aAbBcCdDeE1", "aAbBcCdDeE1",
			   "a watch page" },
			{ "https://www.youtube.com/watch?v=aAbBcCdDeE1&t=42s",
			   "aAbBcCdDeE1", "a watch page with a timestamp" },
			{ "https://youtu.be/aAbBcCdDeE1", "aAbBcCdDeE1",
			   "a short link" },
			// **The one this has to work for.** An embedded player on
			// somebody else's site is an iframe loading this url, and that
			// frame's own location is where the id is.
			{ "https://www.youtube.com/embed/aAbBcCdDeE1", "aAbBcCdDeE1",
			   "an embed, which is the iframe case" },
			{ "https://www.youtube-nocookie.com/embed/aAbBcCdDeE1",
			   "aAbBcCdDeE1", "a no-cookie embed" },
			{ "https://m.youtube.com/watch?v=aAbBcCdDeE1", "aAbBcCdDeE1",
			   "the mobile host" },
			{ "https://www.youtube.com/shorts/aAbBcCdDeE1", "aAbBcCdDeE1",
			   "a short" },
			{ "https://www.youtube.com/watch?v=zzzz-____99", "zzzz-____99",
			   "an id using the whole alphabet" },
			// And the refusals. Each would otherwise be hashed, sent and
			// compared as though it were an id.
			{ "https://example.test/watch?v=aAbBcCdDeE1", "",
			   "another site entirely" },
			{ "https://www.youtube.com/embed/", "", "an embed with no id" },
			{ "https://www.youtube.com/results?search_query=cats", "",
			   "a search page" },
			{ "https://www.youtube.com/watch", "", "a watch page with no v" },
			{ "https://www.youtube.com/watch?v=tooshort", "",
			   "an id of the wrong length" },
			{ "https://www.youtube.com/watch?v=has.a.dot1", "",
			   "an id with a character no id has" },
			{ "https://www.youtube.com/", "", "the front page" },
		};
		for (const one &c : cases) {
			const QString got =
			  sponsor_segments::video_id(QUrl(QString::fromLatin1(c.url)));
			check(got == QString::fromLatin1(c.want),
			       QString("%1 -> %2")
			           .arg(QString::fromLatin1(c.what),
			                 got.isEmpty() ? QStringLiteral("(none)") : got));
		}
	}

	section("what goes out is a four-character prefix and nothing else");
	{
		// **Pinned against two other implementations.** The expected values
		// were taken from python's hashlib and confirmed with sha256sum, so
		// this compares Qt's digest against somebody else's rather than
		// against itself -- a vector generated from the code under test would
		// be one witness asked twice.
		check(sponsor_segments::hash_prefix("aAbBcCdDeE1") == "3ce9",
		       QString("aAbBcCdDeE1 -> %1, against python's 3ce9e653")
		           .arg(sponsor_segments::hash_prefix("aAbBcCdDeE1")));
		check(sponsor_segments::hash_prefix("dQw4w9WgXcQ") == "5f6b",
		       QString("dQw4w9WgXcQ -> %1, against python's 5f6b0b4e")
		           .arg(sponsor_segments::hash_prefix("dQw4w9WgXcQ")));
		check(sponsor_segments::hash_prefix("zzzz-____99") == "7ba6",
		       QString("zzzz-____99 -> %1, against python's 7ba615b5")
		           .arg(sponsor_segments::hash_prefix("zzzz-____99")));
		// Four characters, which is the whole privacy argument: it is shared
		// by about one video in sixteen thousand.
		check(sponsor_segments::hash_prefix("aAbBcCdDeE1").size() == 4,
		       "the prefix is four characters");
		// And nothing for something that is not an id, so a bad url cannot
		// become a request.
		check(sponsor_segments::hash_prefix("not-an-id").isEmpty() &&
		          sponsor_segments::hash_prefix("").isEmpty(),
		       "and empty for anything that is not an id");
	}

	section("the answer carries other videos, and only ours is read");
	{
		// A real answer's shape: the prefix matched three videos. Ours is in
		// the middle, so neither "takes the first" nor "takes the last" can
		// pass by accident.
		const QByteArray answer = R"JSON(
[
 {"videoID":"otherVid001","segments":[
   {"category":"sponsor","actionType":"skip","segment":[1.0,999.0],
    "UUID":"a"}]},
 {"videoID":"aAbBcCdDeE1","segments":[
   {"category":"sponsor","actionType":"skip","segment":[30.5,45.25],
    "UUID":"b"},
   {"category":"interaction","actionType":"skip","segment":[10.0,12.0],
    "UUID":"c"},
   {"category":"intro","actionType":"skip","segment":[0.0,5.0],
    "UUID":"d"},
   {"category":"sponsor","actionType":"mute","segment":[60.0,70.0],
    "UUID":"e"},
   {"category":"sponsor","actionType":"skip","segment":[80.0,80.0],
    "UUID":"f"},
   {"category":"sponsor","actionType":"skip","segment":[95.0,90.0],
    "UUID":"g"}]},
 {"videoID":"otherVid002","segments":[
   {"category":"sponsor","actionType":"skip","segment":[2.0,888.0],
    "UUID":"h"}]}
]
)JSON";
		const QList<sponsor_segment> segs =
		  sponsor_segments::parse(answer, "aAbBcCdDeE1");

		check(segs.size() == 2,
		       QString("two of ours survive the filters (%1)").arg(segs.size()));
		// **Sorted, and the other videos' times are nowhere in it.** 999 and
		// 888 are the tells: either appearing would mean a jump taken from
		// somebody else's video.
		bool foreign = false;
		for (const sponsor_segment &s : segs)
			if (s.to > 500)
				foreign = true;
		check(!foreign, "and no segment from another video is among them");
		check(segs.size() == 2 && segs.at(0).from == 10.0 &&
		          segs.at(0).category == "interaction",
		       QString("sorted by start, the interaction first (%1)")
		           .arg(segs.isEmpty() ? -1.0 : segs.at(0).from));
		check(segs.size() == 2 && segs.at(1).from == 30.5 &&
		          segs.at(1).to == 45.25,
		       "and the sponsor second, with its times intact");

		// Each dropped for its own reason, asserted by what is absent: intro
		// is not an auto-skip category, mute is not a skip, and the
		// zero-length and reversed ones are not segments.
		for (const sponsor_segment &s : segs) {
			check(s.category != "intro", "an intro is not skipped");
			check(s.to > s.from, "and every kept segment moves forward");
		}

		// A video the answer does not mention at all.
		check(sponsor_segments::parse(answer, "zzzz-____99").isEmpty(),
		       "a video not in the answer gets nothing");
		// And nonsense in, nothing out -- an error page rather than JSON is
		// the ordinary failure.
		check(sponsor_segments::parse("<html>nope</html>", "aAbBcCdDeE1")
		          .isEmpty(),
		       "a body that is not JSON gets nothing");
	}

	section("what would be asked, without anything being asked");
	{
		// The url is checkable without a request, which is the only way to
		// assert what goes out without sending it.
		const QUrl q = sponsor_skip::query_url("3ce9");
		check(q.scheme() == "https",
		       QString("https (%1)").arg(q.scheme()));
		check(q.path().endsWith("/3ce9"),
		       QString("the path carries the prefix and nothing else (%1)")
		           .arg(q.path()));
		// **The video id must not be in it anywhere.** That is the whole
		// design, so it is asserted against the url as a string rather than
		// against the part somebody remembered to check.
		check(!q.toString().contains("aAbBcCdDeE1"),
		       QString("and no video id appears in it (%1)")
		           .arg(q.toString().left(80)));
		for (const QString &c : sponsor_segments::skipped_categories())
			check(q.query().contains(c),
			       QString("the categories asked for include %1").arg(c));
	}

	section("the frame worker skips, run in an engine rather than read");
	{
		// **A page, stubbed to the few things the worker touches.** The rule
		// for where to jump lives only in this script -- there is no copy of
		// it in C++ to test instead -- so the test runs it.
		QJSEngine eng;
		const QString harness = QStringLiteral(R"JS(
var posted = [];
var listeners = {};
var vid = {
  currentTime: 0, __l: {},
  addEventListener: function (n, f) { this.__l[n] = f; },
  fire: function () {
    if (this.__l.timeupdate) this.__l.timeupdate({ target: this });
  }
};
var document = {
  documentElement: {},
  getElementsByTagName: function (n) { return n === 'video' ? [vid] : []; }
};
var window = {
  addEventListener: function (n, f) { listeners[n] = f; },
  top: { postMessage: function (m) { posted.push(m); } }
};
var location = { href: 'https://www.youtube.com/watch?v=aAbBcCdDeE1' };
function deliver(json) {
  listeners.message({ data: { __hydra_sponsor_segs: json } });
}
)JS");
		check(!eng.evaluate(harness).isError(), "the stub page evaluates");
		const QJSValue loaded = eng.evaluate(sponsor_skip::frame_source());
		check(!loaded.isError(),
		       QString("and the worker (%1)")
		           .arg(loaded.isError() ? loaded.toString()
		                                  : QStringLiteral("no error")));

		// It asks upward, with its own frame's url -- which is how an
		// embedded player is reached at all.
		check(eng.evaluate("posted.length").toInt() == 1,
		       QString("it posts one question to the top frame (%1)")
		           .arg(eng.evaluate("posted.length").toInt()));
		check(eng.evaluate("String(posted[0].__hydra_sponsor_ask)").toString()
		          .contains("aAbBcCdDeE1"),
		       QString("naming its own url (%1)")
		           .arg(eng.evaluate("String(posted[0].__hydra_sponsor_ask)")
		                    .toString()));

		// Two segments that abut, which is where the chaining matters.
		eng.evaluate("deliver('[{\"f\":10,\"t\":20},"
		              "{\"f\":20,\"t\":25}]')");
		eng.evaluate("vid.currentTime = 12; vid.fire();");
		check(eng.evaluate("vid.currentTime").toNumber() == 25.0,
		       QString("inside the first, it lands past the second (%1)")
		           .arg(eng.evaluate("vid.currentTime").toNumber()));

		eng.evaluate("vid.currentTime = 30; vid.fire();");
		check(eng.evaluate("vid.currentTime").toNumber() == 30.0,
		       QString("outside any segment, it leaves the player alone (%1)")
		           .arg(eng.evaluate("vid.currentTime").toNumber()));

		// The lead-in: a player reports where it has reached, so a position a
		// fraction before the start is already about to be inside.
		eng.evaluate("vid.currentTime = 9.9; vid.fire();");
		check(eng.evaluate("vid.currentTime").toNumber() == 25.0,
		       QString("a fraction before the start counts as inside (%1)")
		           .arg(eng.evaluate("vid.currentTime").toNumber()));
		eng.evaluate("vid.currentTime = 9.0; vid.fire();");
		check(eng.evaluate("vid.currentTime").toNumber() == 9.0,
		       QString("a second before it does not (%1)")
		           .arg(eng.evaluate("vid.currentTime").toNumber()));

		// An empty answer is the ordinary one -- nobody has submitted
		// anything for most videos -- and it must leave the player untouched.
		QJSEngine quiet;
		check(!quiet.evaluate(harness).isError() &&
		          !quiet.evaluate(sponsor_skip::frame_source()).isError(),
		       "a second page evaluates");
		quiet.evaluate("deliver('[]')");
		quiet.evaluate("vid.currentTime = 12; vid.fire();");
		check(quiet.evaluate("vid.currentTime").toNumber() == 12.0,
		       QString("with no segments, nothing moves (%1)")
		           .arg(quiet.evaluate("vid.currentTime").toNumber()));
		// And a body that is not JSON, which is what a mangled relay would
		// hand it.
		quiet.evaluate("deliver('<html>')");
		quiet.evaluate("vid.currentTime = 12; vid.fire();");
		check(quiet.evaluate("vid.currentTime").toNumber() == 12.0,
		       "nor with an answer that is not JSON");
	}

	section("off by default means no request, and the server counts");
	{
		// **The property the whole design rests on.** A test with no network
		// cannot tell a request that was never made from one that failed, so
		// the service is pointed at a local server that counts what it is
		// asked for. Zero is then zero.
		echo_server srv;
		srv.content_type = "application/json";
		const QString base = srv.start();
		check(!base.isEmpty(), "a local stand-in for the service");
		srv.files["/api/skipSegments/3ce9"] =
		  "[{\"videoID\":\"aAbBcCdDeE1\",\"segments\":["
		  "{\"category\":\"sponsor\",\"actionType\":\"skip\","
		  "\"segment\":[30.0,45.0]}]}]";
		qputenv("HYDRA_SPONSOR_SERVICE", base.toUtf8());

		policy_engine pol;
		sponsor_skip blocked(&pol);
		blocked.set_page_host("example.test");
		QStringList said;
		QObject::connect(&blocked, &sponsor_skip::answer,
		                  [&said](const QString &, const QString &json) {
			said << json;
		});
		blocked.ask("t1", "https://www.youtube.com/watch?v=aAbBcCdDeE1");

		// **Synchronously empty**, which is only possible if nothing was
		// fetched: the answer arrives before the event loop has turned.
		check(said.size() == 1 && said.first() == "[]",
		       QString("the default answers empty at once (%1)")
		           .arg(said.join("|")));
		spin(300);
		check(srv.files_served == 0,
		       QString("and the server was asked for nothing (%1)")
		           .arg(srv.files_served));

		// Now allowed for this site, which is the only thing that changes.
		pol.set_setting("example.test", policy::feature::sponsor_skip,
		                 policy::setting::allow);
		sponsor_skip allowed(&pol);
		allowed.set_page_host("example.test");
		QStringList answered;
		QObject::connect(&allowed, &sponsor_skip::answer,
		                  [&answered](const QString &, const QString &json) {
			answered << json;
		});
		allowed.ask("t2", "https://www.youtube.com/watch?v=aAbBcCdDeE1");
		for (int i = 0; i < 100 && answered.isEmpty(); ++i)
			spin(25);
		check(srv.files_served == 1,
		       QString("allowed, it asks once (%1)").arg(srv.files_served));
		check(answered.size() == 1 && answered.first().contains("\"f\":30"),
		       QString("and the segments come back (%1)")
		           .arg(answered.join("|")));

		// A second video sharing the prefix is answered from the same fetch,
		// which is the only efficiency the prefix design hands back.
		answered.clear();
		allowed.ask("t3", "https://www.youtube.com/watch?v=aAbBcCdDeE1");
		for (int i = 0; i < 40 && answered.isEmpty(); ++i)
			spin(25);
		check(srv.files_served == 1,
		       QString("a repeat is answered from the cache (%1)")
		           .arg(srv.files_served));
		qunsetenv("HYDRA_SPONSOR_SERVICE");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
