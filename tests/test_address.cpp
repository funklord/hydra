// What the address bar does with what somebody typed: address or search.
//
// Written against the *leak* rather than the convenience. Getting "search"
// wrong for something that was an address costs a keystroke; getting it wrong
// for an intranet address sends a private hostname to a third party, and that
// cannot be recalled. So the hostile cases here are the ones that look like
// prose and are hosts, and the ones that look like hosts and are prose.
#include "address_input.h"

#include <QCoreApplication>
#include <QString>
#include <QUrl>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static void is_address(const char *s) {
	check(looks_like_address(QString::fromUtf8(s)),
	      QString("\"%1\" is an address").arg(s));
}
static void is_search(const char *s) {
	check(!looks_like_address(QString::fromUtf8(s)),
	      QString("\"%1\" is a search").arg(s));
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("things that are plainly addresses");
	{
		is_address("https://example.com");
		is_address("http://example.com/a/b?c=d");
		is_address("example.com");
		is_address("www.example.co.uk/path");
		is_address("file:///tmp/x.html");
		is_address("magnet:?xt=urn:btih:abc");
		is_address("about:blank");
		is_address("view-source:https://example.com");
		// A scheme wins over a space. Somebody who typed one meant an address,
		// and the engine can encode whatever follows.
		is_address("https://example.com/a b");
	}

	section("things that are plainly searches");
	{
		is_search("weather tomorrow");
		is_search("how do i exit vim");
		is_search("qt webengine print to pdf");
		is_search("");
		is_search("   ");
	}

	section("the hosts that must never reach a search engine");
	{
		// The whole reason this unit exists. Each of these is a real address
		// on somebody's network, and searching for it publishes the name.
		is_address("internal.corp.example/secret");
		is_address("wiki.internal.example");
		is_address("localhost");
		is_address("localhost:8080");
		is_address("127.0.0.1");
		is_address("127.0.0.1:36853");
		is_address("192.168.1.1/admin");
		is_address("[::1]");
		is_address("[::1]:8080");
		is_address("/etc/hosts");
		is_address("./notes.html");
		is_address("~/notes.html");
	}

	section("things with a dot that are not hosts");
	{
		// The mirror case, and the reason a dot alone is not enough: each of
		// these would become `http://...` and load nothing.
		is_search("3.14");
		is_search("1.5x faster");
		is_search("version 2.0 release notes");
		is_search("example.");            // trailing dot, a typo
		is_search("example..com");        // empty label, a typo
		is_search("foo.c");               // one-character last label
		is_search("main.4");              // digits are not a tld
	}

	section("a colon is not always a port");
	{
		is_search("note: buy milk");      // a space, so not a scheme
		is_address("example.com:8080");
		is_address("example.com:8080/x");
		// `foo:bar` has a colon that is neither a port nor a scheme followed by
		// anything host-shaped -- but it *is* scheme-shaped, and the rule says
		// a scheme wins. Recorded as the behaviour rather than defended as
		// ideal: a custom scheme is exactly this shape, and refusing it would
		// break the ones the shell exists to route.
		is_address("foo:bar");
	}

	section("building the search url");
	{
		const QString tmpl = "https://duckduckgo.com/?q=%1";
		// **FullyEncoded, because `toString()` hides the thing under test.**
		// Its default is PrettyDecoded, which renders `%20` back as a space --
		// so the first version of this compared against a decoded string and
		// failed while the url was perfectly correct. What goes on the wire is
		// the encoded form, and that is what to assert on.
		check(search_url("weather tomorrow", tmpl).toString(QUrl::FullyEncoded) ==
		        "https://duckduckgo.com/?q=weather%20tomorrow",
		      "spaces are percent-encoded");
		check(search_url("a&b=c", tmpl).toString().contains("%26"),
		      "an ampersand cannot escape into the query it is placed in");
		check(search_url("  padded  ", tmpl).toString() ==
		        "https://duckduckgo.com/?q=padded",
		      "surrounding space is trimmed rather than encoded");
		check(!search_url("x", "https://example.com/search").isValid(),
		      "a template with no %1 is refused, not searched for nothing");
		check(search_url("x", "https://s.example/?q=%1").isValid(),
		      "and any template carrying one is accepted, whoever wrote it");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
