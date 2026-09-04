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
		// **A port and a path together, which is where this broke.** The port
		// was stripped before the path, so the candidate port was `8753/admin`
		// -- not digits, nothing stripped -- and the host came out as
		// `127.0.0.1:8753`, which is not an address. A hostname survived by
		// accident, still looking domain-shaped with the port attached, so only
		// bare IPs fell through: the router page and the printer page, which
		// are exactly what somebody types with a port and a path, and exactly
		// what must never reach a search engine.
		is_address("127.0.0.1:8753/typed");
		is_address("192.168.1.1:631/printers");
		is_address("[::1]:8080/status");
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

	{
		section("what `hydra <argument>` was asked to open");
		// The trap this exists for: `QUrl::fromUserInput` invents a `file:`
		// scheme for a bare path, so a classifier built on the parsed url
		// cannot tell `./tree.txt` from `file:///home/me/doc.html`.
		check(!argument_url("./tree.txt").isValid(),
		      "a relative path is a tree, not a page");
		check(!argument_url("/home/me/tree.txt").isValid(),
		      "and so is an absolute one");
		// **Pinned as it stands rather than as it ought to be.** A bare
		// dotted name that does not exist in the working directory has
		// always been guessed as a host -- `hydra tree.txt` goes to
		// `http://tree.txt` -- because `fromUserInput` reads `.txt` as a
		// top-level domain. That predates the file: change and is not
		// endorsed here; it is asserted so that changing it is a decision
		// somebody makes rather than something that happens.
		check(argument_url("tree.txt").isValid() &&
		       argument_url("tree.txt").scheme() == "http",
		      "a bare dotted name that does not exist is guessed as a host");
		check(argument_url("file:///home/me/doc.html").isValid(),
		      "a file: uri is a page, which is what a file manager hands over");
		check(argument_url("file:///home/me/doc.html").scheme() == "file",
		      "and it keeps its scheme rather than being re-guessed");
		check(argument_url("https://example.com/").isValid(),
		      "https is a page, as before");
		check(argument_url("http://example.com/").isValid(),
		      "and http");
		check(!argument_url("").isValid(),
		      "nothing is not a page");
		// The older rule, left alone: an argument that names a file that
		// exists is a tree even when it parses as a url.
		check(!argument_url(QCoreApplication::applicationFilePath()).isValid(),
		      "an argument naming an existing file stays a tree path");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
