// The gate between an untrusted page and the vault (architecture doc §13.3).
//
// This class exists to say no. Its header lists four rules — the page does not
// choose the origin, autofill answers to the policy engine, HTTPS only, nothing
// held longer than the fill that asked — and nothing checked that any of them
// held. A gate whose refusals are untested is a gate in name.
//
// The delivery half needs a connected, paired KeePassXC and lives in
// `tests/live/try_keepass.cpp`. Everything here is the refusal half, which is
// the half that matters when it is wrong.
#include "autofill_controller.h"
#include "keepass_protocol.h"
#include "policy_engine.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	// No bridge at all. Every gate before "is KeePassXC there" can be exercised
	// without one, and the last gate is then the one that reports — which is
	// also how a first run behaves before anyone has paired anything.
	policy_engine policy;

	section("the page does not choose the origin");
	{
		autofill_controller a(nullptr, &policy);
		a.set_page_origin("https://bank.example");

		check(a.blocked_reason("https://bank.example").contains("KeePassXC"),
		      "the page's own origin passes the origin gate");
		check(a.blocked_reason("https://evil.example").contains("Origin mismatch"),
		      "another site's origin does not");
		check(a.blocked_reason("").contains("Origin mismatch"),
		      "and neither does no origin at all");
		check(a.blocked_reason("https://bank.example.evil.com")
		          .contains("Origin mismatch"),
		      "a lookalike that merely starts the same is refused");
		check(a.blocked_reason("https://bank.example/login")
		          .contains("Origin mismatch"),
		      "and so is the same site with a path — an origin is not a url");
	}

	section("a navigation abandons whatever was in flight");
	{
		autofill_controller a(nullptr, &policy);
		a.set_page_origin("https://one.example");
		check(a.page_origin() == "https://one.example", "the shell sets the origin");
		a.set_page_origin("https://two.example");
		check(a.blocked_reason("https://one.example").contains("Origin mismatch"),
		      "after navigating, the previous page's origin is a stranger");
	}

	section("https only, and it is on by default");
	{
		autofill_controller a(nullptr, &policy);
		a.set_page_origin("http://plain.example");
		check(a.blocked_reason("http://plain.example").contains("HTTPS"),
		      "a plain-http page is refused by default");

		a.set_https_only(false);
		check(!a.blocked_reason("http://plain.example").contains("HTTPS"),
		      "and allowed when the user turns the requirement off");

		// A file:// url has no host, so it never reaches the https check — it is
		// refused one gate earlier, for having no origin worth the name. Worth
		// pinning because the *reason* differs from what one would guess, and
		// the reason is what the key icon shows the user.
		autofill_controller b(nullptr, &policy);
		b.set_page_origin("file:///home/someone/page.html");
		check(b.blocked_reason("file:///home/someone/page.html")
		          .contains("No usable origin"),
		      QString("a file:// page is refused for having no host (%1)")
		          .arg(b.blocked_reason("file:///home/someone/page.html")));
	}

	section("the policy engine governs it like anything else");
	{
		autofill_controller a(nullptr, &policy);
		a.set_page_origin("https://blocked.example");
		policy.set_setting("blocked.example", policy::feature::autofill,
		                    policy::setting::block);
		check(a.blocked_reason("https://blocked.example").contains("blocked for this site"),
		      "a site the user blocked autofill for is refused");

		policy.set_setting("blocked.example", policy::feature::autofill,
		                    policy::setting::allow);
		check(!a.blocked_reason("https://blocked.example").contains("blocked for this site"),
		      "and allowed once the user says so");

		// Subdomains follow the policy engine's own pattern language rather than
		// any rule of this class's — one place decides what a site is. That
		// language is `*`, `*.domain`, or an exact host, so an exact rule does
		// **not** reach a subdomain, and expecting otherwise here would have been
		// a rule about sites written in the wrong file.
		autofill_controller c(nullptr, &policy);
		c.set_page_origin("https://sub.blocked.example");
		policy.set_setting("blocked.example", policy::feature::autofill,
		                    policy::setting::block);
		check(!c.blocked_reason("https://sub.blocked.example").contains("blocked for this site"),
		      "an exact-host rule does not reach a subdomain");
		policy.set_setting("*.blocked.example", policy::feature::autofill,
		                    policy::setting::block);
		check(c.blocked_reason("https://sub.blocked.example").contains("blocked for this site"),
		      "and the wildcard pattern is how a user covers one");
	}

	section("the order of refusals");
	{
		// A page that is wrong in several ways at once should be told about the
		// most fundamental one. The origin gate comes first because it is the
		// only one that says the *page* is misbehaving rather than that the
		// situation is unsuitable.
		autofill_controller a(nullptr, &policy);
		a.set_page_origin("https://real.example");
		policy.set_setting("real.example", policy::feature::autofill,
		                    policy::setting::block);
		check(a.blocked_reason("http://evil.example").contains("Origin mismatch"),
		      "origin first, before https or policy");
	}

	section("a refusal is a refusal, not a quiet nothing");
	{
		autofill_controller a(nullptr, &policy);
		QSignalSpy refused(&a, &autofill_controller::refused);
		QSignalSpy ready(&a, &autofill_controller::credentials_ready);

		a.set_page_origin("https://site.example");
		a.request_credentials("https://evil.example");
		check(refused.count() == 1, "the page is told it was refused");
		check(ready.count() == 0, "and no credentials are delivered");
		check(refused.takeFirst().at(0).toString().contains("Origin mismatch"),
		      "with the reason, which is what the key icon shows");

		a.request_credentials("https://site.example");
		check(ready.count() == 0,
		      "and a request that passes the gate still delivers nothing without "
		      "a paired KeePassXC");
	}

	section("\"no logins found\" is an answer, not an error");
	{
		// Found against a real KeePassXC, and only once a stored pairing made
		// `get-logins` reachable without a human confirming a dialog. A url the
		// vault has no entry for comes back as error 15 -- so routing it like
		// any other error meant no `logins` signal was ever emitted for it, and
		// the fill that asked stayed pending until the page navigated. That is
		// every site not in the vault, which is nearly all of them.
		//
		// The routing itself lives in `keepass_bridge::handle`, behind a socket
		// and a handshake, so `try_keepass` is what proves it end to end. What
		// is pinned here is the fact it depends on: which code, read how.
		QJsonObject no_logins;
		no_logins.insert("error", "No logins found");
		no_logins.insert("errorCode", "15");
		QString message;
		check(keepass_protocol::is_error(no_logins, &message),
		      "KeePassXC reports \"nothing stored\" as an error");
		check(keepass_protocol::error_code(no_logins) ==
		          keepass_protocol::no_logins_found,
		      QString("and the code is the one we single out (%1)")
		          .arg(keepass_protocol::error_code(no_logins)));
		check(keepass_protocol::parse_logins(no_logins).isEmpty(),
		      "and it parses to no entries rather than a bad one");

		// The code travels as a *string*. A numeric read of a JSON string is 0,
		// which is also this function's answer for "no error", so getting that
		// wrong would silently turn every error into no error.
		QJsonObject numeric;
		numeric.insert("error", "No logins found");
		numeric.insert("errorCode", 15);
		check(keepass_protocol::error_code(numeric) == 0,
		      "a code sent as a number rather than a string reads as 0, not 15");

		QJsonObject other;
		other.insert("error", "Database not opened");
		other.insert("errorCode", "1");
		check(keepass_protocol::error_code(other) == 1,
		      "a different failure keeps its own code and stays an error");

		QJsonObject fine;
		fine.insert("success", "true");
		fine.insert("entries", QJsonArray());
		check(!keepass_protocol::is_error(fine, &message) &&
		          keepass_protocol::error_code(fine) == 0,
		      "and a success reply has no code at all");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
