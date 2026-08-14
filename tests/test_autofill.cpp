// The gate between an untrusted page and the vault (architecture doc sec 13.3).
//
// This class exists to say no. Its header lists four rules -- the page does not
// choose the origin, autofill answers to the policy engine, HTTPS only, nothing
// held longer than the fill that asked -- and nothing checked that any of them
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

// Two logins for one site, which is the case the picker exists for.
static QList<credential> two_entries() {
	QList<credential> two;
	two << credential{ "Work", "alice", "pw-work" }
	    << credential{ "Personal", "alice2", "pw-home" };
	return two;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	// No bridge at all. Every gate before "is KeePassXC there" can be exercised
	// without one, and the last gate is then the one that reports -- which is
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

		// A file:// url has no host, so it never reaches the https check -- it is
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
		// any rule of this class's -- one place decides what a site is. That
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

	section("more than one login is a question, and the passwords stay here");
	{
		// The delivery half needs a paired KeePassXC and lives in try_keepass.
		// What is checked here is the part that runs before any of that: what
		// the controller does with a set of entries, and above all what it does
		// *not* put across the boundary.
		//
		// The old arrangement handed the page every match and let the injected
		// script decide; the script's answer to more than one was to fill
		// nothing. So three logins for a site sent three passwords into the page
		// and used none of them -- no fill, and credentials delivered for a fill
		// that never happened.
		autofill_controller a(nullptr, &policy);
		QSignalSpy ready(&a, &autofill_controller::credentials_ready);
		QSignalSpy asked(&a, &autofill_controller::choice_needed);
		QSignalSpy refused(&a, &autofill_controller::refused);
		a.set_page_origin("https://site.example");

		a.offer_for_test(two_entries());

		check(asked.count() == 1, "two matches asks which");
		check(ready.count() == 0, "and delivers nothing until it is answered");
		const QStringList labels = asked.takeFirst().at(0).toStringList();
		check(labels.size() == 2, "with one label per entry");
		// The claim the whole design rests on.
		check(!labels.join('\n').contains("pw-work") &&
		          !labels.join('\n').contains("pw-home"),
		      "and no password in any of them");
		check(labels.at(0).contains("alice") && labels.at(0).contains("Work"),
		      QString("naming what tells the accounts apart (%1)").arg(labels.at(0)));

		a.choose(1);
		check(ready.count() == 1, "choosing delivers");
		const QString json = ready.takeFirst().at(0).toString();
		check(json.contains("pw-home"), "the one that was chosen");
		check(!json.contains("pw-work"),
		      "and only that one -- the other never reaches the page");

		// Answering twice must not fill twice: a double-click on the picker, or
		// a second dialog raised over the first.
		a.choose(0);
		check(ready.count() == 0, "and a second answer to the same question fills nothing");
	}
	{
		autofill_controller a(nullptr, &policy);
		QSignalSpy ready(&a, &autofill_controller::credentials_ready);
		QSignalSpy asked(&a, &autofill_controller::choice_needed);
		a.set_page_origin("https://site.example");

		QList<credential> one;
		one << credential{ "Work", "alice", "pw-work" };
		a.offer_for_test(one);
		check(asked.count() == 0, "one match asks nothing");
		check(ready.count() == 1, "and fills straight away");

		// Dismissing is a legitimate answer, not an error.
		autofill_controller b(nullptr, &policy);
		QSignalSpy bready(&b, &autofill_controller::credentials_ready);
		QSignalSpy brefused(&b, &autofill_controller::refused);
		b.set_page_origin("https://site.example");
		b.offer_for_test(two_entries());
		b.choose(-1);
		check(bready.count() == 0, "dismissing the picker fills nothing");
		check(brefused.count() == 0, "and is not reported as a refusal");

		// And the case the origin gate cannot see: the request passed the gate
		// when it was made, and the page moved while the picker was open.
		autofill_controller c(nullptr, &policy);
		QSignalSpy cready(&c, &autofill_controller::credentials_ready);
		QSignalSpy crefused(&c, &autofill_controller::refused);
		c.set_page_origin("https://site.example");
		c.offer_for_test(two_entries());
		c.set_page_origin("https://elsewhere.example");
		c.choose(0);
		check(cready.count() == 0,
		      "a choice answered after the page moved fills nothing");
		check(crefused.count() == 1 &&
		          crefused.takeFirst().at(0).toString().contains("changed while"),
		      "and says why, since the user did click something");
	}

	section("the key hears about a page whether or not the fill is allowed");
	{
		// The affordance sec 13.2 asks for is only useful if it appears on the
		// pages that have something to say. A `requested` that fired after the
		// gate would show the key exactly when everything worked and hide it
		// when autofill was blocked -- which is the page where a user needs to
		// see it and read why.
		autofill_controller a(nullptr, &policy);
		QSignalSpy asked(&a, &autofill_controller::requested);
		QSignalSpy refused(&a, &autofill_controller::refused);
		a.set_page_origin("https://site.example");

		a.request_credentials("https://evil.example");
		check(asked.count() == 1,
		      "a request from the wrong origin still raises the key");
		check(refused.count() == 1, "and is refused");

		policy.set_setting("site.example", policy::feature::autofill,
		                    policy::setting::block);
		a.request_credentials("https://site.example");
		check(asked.count() == 2,
		      "and so does one the user has blocked autofill for");
		policy.set_setting("site.example", policy::feature::autofill,
		                    policy::setting::unset);
	}

	section("no logins at all is said, not left silent");
	{
		autofill_controller a(nullptr, &policy);
		QSignalSpy ready(&a, &autofill_controller::credentials_ready);
		QSignalSpy refused(&a, &autofill_controller::refused);
		a.set_page_origin("https://site.example");
		a.offer_for_test({});
		check(ready.count() == 0, "an empty vault answer delivers nothing");
		check(refused.count() == 1 &&
		          refused.takeFirst().at(0).toString().contains("No credentials"),
		      "and says so, because an empty form looks the same as a broken bridge");
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

	section("set-login: the message shape");
	{
		// Create: no uuid given.
		const QJsonObject create = keepass_protocol::set_login_request(
		    "https://bank.example", "alice", "hunter2", QString(), "assoc-1", "idkey-b64");
		check(create.value("action").toString() == "set-login",
		      "the action is set-login");
		check(create.value("url").toString() == "https://bank.example",
		      "the url travels");
		check(create.value("submitUrl").toString() == "https://bank.example",
		      "submitUrl is the same url -- there is only the one we were given");
		check(create.value("login").toString() == "alice", "the login travels");
		check(create.value("password").toString() == "hunter2",
		      "the password travels");
		check(!create.contains("uuid"), "no uuid means create, and none is sent");

		const QJsonArray keys = create.value("keys").toArray();
		check(keys.size() == 1, "the association proof is a one-element keys array, "
		                        "the same shape get-logins sends");
		check(keys.first().toObject().value("id").toString() == "assoc-1" &&
		          keys.first().toObject().value("key").toString() == "idkey-b64",
		      "carrying the same assoc id and key get-logins does");

		// Update: uuid given.
		const QJsonObject update = keepass_protocol::set_login_request(
		    "https://bank.example", "alice", "hunter3", "entry-uuid-1", "assoc-1",
		    "idkey-b64");
		check(update.value("uuid").toString() == "entry-uuid-1",
		      "a non-empty uuid means update, and travels on the wire");
	}

	section("generate-password: the request has nothing to configure");
	{
		const QJsonObject req = keepass_protocol::generate_password_request();
		check(req.value("action").toString() == "generate-password",
		      "the action is generate-password");
		check(req.size() == 1,
		      "and there is nothing else in it -- we ask, KeePassXC decides, "
		      "per its own configured policy (§13.1)");
	}

	section("generate-password: both known reply shapes");
	{
		// Shape one: a bare "password" field.
		QJsonObject direct;
		direct.insert("success", "true");
		direct.insert("password", "Tr0ub4dor&3");
		check(keepass_protocol::parse_generated_password(direct) == "Tr0ub4dor&3",
		      "a bare password field is read directly");

		// Shape two: an "entries" array, the same shape get-logins uses.
		QJsonObject viaEntries;
		viaEntries.insert("success", "true");
		QJsonArray entries;
		QJsonObject entry;
		entry.insert("password", "correct-horse-battery-staple");
		entries.append(entry);
		viaEntries.insert("entries", entries);
		check(keepass_protocol::parse_generated_password(viaEntries) ==
		          "correct-horse-battery-staple",
		      "or an entries array is read the same way get-logins reads one -- "
		      "which shape a given KeePassXC sends was not something this could "
		      "verify offline (see keepass_protocol.h), so both are handled");

		// Neither shape present.
		QJsonObject empty;
		empty.insert("success", "true");
		check(keepass_protocol::parse_generated_password(empty).isEmpty(),
		      "neither shape present parses to an empty string, not a crash");

		// An error reply, same two shapes error() already covers.
		QJsonObject failed;
		failed.insert("error", "Action cancelled or timed out");
		failed.insert("errorCode", "13");
		check(keepass_protocol::parse_generated_password(failed).isEmpty(),
		      "an error reply yields no password, whichever shape it would "
		      "otherwise have carried one in");
	}

	section("set-login: parsing the reply");
	{
		// No "error" key at all -- is_error() treats the key's mere presence as
		// failure regardless of its value (see the "code travels as a string"
		// test above), which a real reply's own `"error": ""` would trip if
		// this test copied it verbatim. A success reply this codebase already
		// agrees is a success looks like the "fine" object further up.
		QJsonObject ok;
		ok.insert("success", "true");
		check(keepass_protocol::parse_set_login(ok), "a success reply parses ok");

		QJsonObject refused;
		refused.insert("error", "public key not found");
		refused.insert("errorCode", "1");
		check(!keepass_protocol::parse_set_login(refused),
		      "an error reply does not");
	}


	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
