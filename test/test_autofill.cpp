// The gate between an untrusted page and the vault (architecture doc sec 13.3).
//
// This class exists to say no. Its header lists four rules -- the page does not
// choose the origin, autofill answers to the policy engine, HTTPS only, nothing
// held longer than the fill that asked -- and nothing checked that any of them
// held. A gate whose refusals are untested is a gate in name.
//
// The delivery half needs a connected, paired KeePassXC and lives in
// `test/live/try_keepass.cpp`. Everything here is the refusal half, which is
// the half that matters when it is wrong.
#include "autofill_controller.h"
#include "keepass_bridge.h"
#include "keepass_protocol.h"
#include "policy_engine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <cstdio>

// The reference the nonce increment is compared against -- see the section
// that uses it for why a hand-written one is checked rather than trusted.
#include <sodium.h>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Two logins for one site, which is the case the picker exists for.
static QList<credential> two_entries() {
	QList<credential> two;
	two << credential{ "Work", "alice", "pw-work", QString() }
	    << credential{ "Personal", "alice2", "pw-home", QString() };
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
		one << credential{ "Work", "alice", "pw-work", QString() };
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


	section("where KeePassXC's socket is looked for");
	{
		// **Pinned against the algorithm the other end actually uses**, which
		// was traced rather than assumed: keepassxc-proxy 2.7.10 under
		// `strace -e trace=file` probes
		// `<runtime>/org.keepassxc.KeePassXC.BrowserServer` first, where
		// `<runtime>` is Qt's RuntimeLocation.
		//
		// This used to read `XDG_RUNTIME_DIR` directly and fall back to a bare
		// `/tmp`, which differs from Qt's answer in two configurations: the
		// variable unset, where Qt uses `$TMPDIR/runtime-$USER`; and the
		// variable set to a directory that is not 0700, which Qt rejects and
		// the raw read does not. Either way the socket could not be found.
		//
		// Asserted against `QStandardPaths` rather than against a literal,
		// because a literal here would be this file guessing at the same thing
		// the code guesses at -- two copies of one assumption, agreeing with
		// each other and with nothing else.
		const QString runtime =
		  QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
		const QString sock = keepass_bridge::socket_path();
		check(sock == runtime + "/org.keepassxc.KeePassXC.BrowserServer",
		      QString("the socket is under Qt's runtime directory (%1)").arg(sock));
		// **When it does not answer, say which directory it refused**, because
		// "Qt always answers" is the one thing a failure here has already
		// disproved, and the message as it stood sent nobody anywhere.
		//
		// Qt returns an EMPTY string -- not a fallback -- when the directory
		// it would use carries any group or other permission bit. Measured
		// against 6.8.2: absent, 0700 and 2700 all answer; 0750 and 0770
		// answer with nothing. `make test` now creates that directory with
		// its mode stated, so a harness run cannot meet it -- but a suite run
		// directly gets no such repair, and running a binary directly is what
		// debugging one means.
		//
		// Recomputing Qt's path here would be two copies of one assumption if
		// it were an assertion. It is a message on a failure that has already
		// happened, which is the one place a second opinion costs nothing.
		if (runtime.isEmpty()) {
			const QString guess = QDir::tempPath() + "/runtime-"
			                       + QString::fromLocal8Bit(qgetenv("USER"));
			const QFileInfo fi(guess);
			QString why = "and it does not exist";
			if (fi.exists()) {
				const QFile::Permissions p = fi.permissions();
				why = (p & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
				             | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther))
				        ? "which carries group or other permission bits; Qt "
				          "refuses those and returns nothing"
				        : "which looks correct, so the cause is elsewhere";
			}
			std::printf("  ..    Qt would have used %s, %s\n",
			             qPrintable(guess), qPrintable(why));
		}
		check(!runtime.isEmpty(),
		      "which Qt always answers, with or without XDG_RUNTIME_DIR");
		check(sock.endsWith("/org.keepassxc.KeePassXC.BrowserServer"),
		      "and carries the name KeePassXC publishes");
	}

	section("the nonce increment, against libsodium's own");
	{
		// **`increment_nonce` had no test at all**, and the matcher section
		// above only ever increments a zero nonce -- so the carry, which is
		// the whole of the function, was never exercised. Its own comment
		// says why that matters: "getting the carry wrong produces a nonce
		// reuse, which is the one failure this protocol cannot tolerate".
		//
		// The header also says it behaves "exactly as libsodium's
		// sodium_increment does", and libsodium is linked here, so the claim
		// is checkable rather than merely stated. That is the point of the
		// comparison: a hand-written increment agreeing with itself proves
		// nothing, and these fixtures were chosen so that a wrong carry
		// cannot pass.
		struct nonce_case {
			QByteArray  n;
			const char *what;
		};
		QByteArray all_ff(24, '\xff');
		QByteArray low_ff(24, '\0');  low_ff[0] = '\xff';
		QByteArray two_ff(24, '\0');  two_ff[0] = '\xff'; two_ff[1] = '\xff';
		QByteArray mid_ff(24, '\0');  mid_ff[7] = '\xff';
		QByteArray cascade(23, '\xff'); cascade.append('\x01');
		QByteArray mixed;
		quint32 seed = 0xc0ffee11;
		for (int i = 0; i < 24; ++i) {
			seed = seed * 1103515245u + 12345u;
			mixed += char((seed >> 16) & 0xFF);
		}
		const QList<nonce_case> cases = {
			{ QByteArray(24, '\0'), "a zero nonce" },
			{ low_ff,  "a carry out of the first byte" },
			{ two_ff,  "a carry through two bytes" },
			{ cascade, "a carry through twenty-three bytes" },
			{ all_ff,  "every byte set, which wraps to zero" },
			{ mid_ff,  "0xff in the middle, where nothing should carry" },
			{ mixed,   "an arbitrary nonce" },
		};
		for (const nonce_case &c : cases) {
			QByteArray reference = c.n;
			sodium_increment(reinterpret_cast<unsigned char *>(reference.data()),
			                  size_t(reference.size()));
			const QByteArray ours = keepass_protocol::increment_nonce(c.n);
			check(ours == reference,
			      QString("%1: %2, and libsodium says %3")
			          .arg(c.what, QString::fromLatin1(ours.toHex()),
			                QString::fromLatin1(reference.toHex())));
		}

		// Little-endian, stated separately because it is the thing a reader
		// is most likely to assume the other way round: the FIRST byte is the
		// one that moves.
		QByteArray one(24, '\0');
		const QByteArray stepped = keepass_protocol::increment_nonce(one);
		check(stepped.at(0) == '\x01' && stepped.mid(1) == QByteArray(23, '\0'),
		      "and it counts from the first byte, not the last");

		// The size is preserved, since a nonce one byte short or long is not
		// a nonce the other side can use.
		check(keepass_protocol::increment_nonce(mixed).size() == mixed.size(),
		      "a nonce keeps its length");
	}

	section("a reply is matched to the request that asked, not to the last one");
	{
		// The defect this closes: the bridge keyed its pending tags by action,
		// so two `get-logins` in flight -- two tabs reaching a login form in
		// the same second -- left the second one's tag in the slot, and the
		// first reply to arrive was delivered under it. The page that asked
		// second got the credentials for the site that asked first.
		//
		// Nothing adversarial is needed and the socket is not involved, which
		// is why the matching is a pure function here rather than something
		// only `try_keepass` can reach.
		using keepass_protocol::match_reply;
		using keepass_protocol::pending_request;
		using keepass_protocol::reply_match;

		// Sequential nonces, as the bridge produces them: one counter,
		// incremented per message.
		QByteArray base(24, '\0');
		const QByteArray n_a = keepass_protocol::increment_nonce(base);
		const QByteArray n_b = keepass_protocol::increment_nonce(n_a);

		QList<pending_request> pending;
		pending << pending_request{ "get-logins", n_a, 11 }
		        << pending_request{ "get-logins", n_b, 22 };

		int which = -1;
		check(match_reply(pending, "get-logins",
		                   keepass_protocol::increment_nonce(n_a), &which)
		          == reply_match::by_incremented_nonce &&
		          which == 0 && pending.at(which).tag == 11,
		      "the first asker's reply goes to the first asker");

		which = -1;
		check(match_reply(pending, "get-logins",
		                   keepass_protocol::increment_nonce(n_b), &which)
		          == reply_match::by_incremented_nonce &&
		          which == 1 && pending.at(which).tag == 22,
		      "and the second asker's to the second");

		// `increment_nonce(n_a) == n_b`, because the nonces are consecutive.
		// So a reply carrying n_b is ambiguous between "the specified reply to
		// A" and "an echo of B's own nonce", and the specified relationship is
		// the one believed. Pinned because it is a deliberate choice and the
		// only case where the two rules can disagree.
		which = -1;
		check(keepass_protocol::increment_nonce(n_a) == n_b,
		      "consecutive requests make the reply rules collide by construction");
		check(match_reply(pending, "get-logins", n_b, &which)
		          == reply_match::by_incremented_nonce &&
		          pending.at(which).tag == 11,
		      "and the incremented nonce is believed over an echoed one");

		// A server that echoes rather than increments. Nothing here has met a
		// real KeePassXC, so both are accepted; `try_keepass` reports which.
		QList<pending_request> one;
		one << pending_request{ "get-logins", n_a, 11 };
		which = -1;
		check(match_reply(one, "get-logins", n_a, &which)
		          == reply_match::by_echoed_nonce && one.at(which).tag == 11,
		      "an echoed nonce matches too, with only one in flight");

		// The no-breakage property, and the reason a strict check was not
		// written: one request in flight is answered whatever the nonce says,
		// including a reply that carries none at all.
		which = -1;
		check(match_reply(one, "get-logins", QByteArray(), &which)
		          == reply_match::by_being_alone && one.at(which).tag == 11,
		      "and a reply with no nonce at all still answers a lone request");

		// Two in flight and a nonce matching neither: deliver nothing. A reply
		// handed to the wrong tag is a password in the wrong page, which is
		// worse than a fill that does not happen.
		which = -1;
		QByteArray stranger(24, '\x7f');
		check(match_reply(pending, "get-logins", stranger, &which)
		          == reply_match::ambiguous && which == -1,
		      "two in flight and an unplaceable nonce is refused, not guessed");

		// What the old action-keyed map got right, and this must keep: a
		// set-login reply does not consume a get-logins request.
		QList<pending_request> mixed;
		mixed << pending_request{ "get-logins", n_a, 11 }
		      << pending_request{ "set-login", n_b, 22 };
		which = -1;
		check(match_reply(mixed, "set-login",
		                   keepass_protocol::increment_nonce(n_b), &which)
		          == reply_match::by_incremented_nonce && mixed.at(which).tag == 22,
		      "a reply only ever matches a request of its own action");
		which = -1;
		check(match_reply(mixed, "test-associate", n_a, &which)
		          == reply_match::none_pending,
		      "and an action nothing asked for matches nothing");
	}

	section("a save updates the entry it came from, rather than adding beside it");
	{
		// `set_login_request` has taken a uuid since it was written, documented
		// as "empty means create", and nothing could ever fill it in:
		// `parse_logins` dropped the uuid KeePassXC sends. So every save was an
		// add. Change a stored password, accept the offer, and the vault holds
		// two entries for the site differing only in the password -- and the
		// next fill asks the person to choose between them with nothing on
		// screen to say which is current.
		QJsonObject entry;
		entry.insert("name", "Work");
		entry.insert("login", "alice");
		entry.insert("password", "pw");
		entry.insert("uuid", "abc123");
		QJsonArray arr;
		arr.append(entry);
		QJsonObject reply;
		reply.insert("entries", arr);
		const QList<credential> parsed = keepass_protocol::parse_logins(reply);
		check(parsed.size() == 1 && parsed.first().uuid == "abc123",
		      "the uuid KeePassXC sends with an entry is kept");

		// An entry with no uuid is still a usable credential -- the field is
		// what makes an update possible, not what makes a fill possible.
		QJsonObject bare;
		bare.insert("login", "bob");
		bare.insert("password", "pw");
		QJsonArray arr2;
		arr2.append(bare);
		QJsonObject reply2;
		reply2.insert("entries", arr2);
		check(keepass_protocol::parse_logins(reply2).size() == 1,
		      "and an entry without one still parses as a credential");

		QList<credential> known;
		known << credential{ "Work", "alice", QString(), "abc123" }
		      << credential{ "Personal", "bob", QString(), "def456" };
		check(keepass_protocol::uuid_for_login(known, "alice") == "abc123",
		      "saving a login the vault already holds names that entry");
		check(keepass_protocol::uuid_for_login(known, "carol").isEmpty(),
		      "a login it does not hold adds a new one");
		check(keepass_protocol::uuid_for_login(known, QString()).isEmpty(),
		      "and an empty login names nothing rather than the first entry");

		// Two entries under one login: nothing here can choose between them,
		// and the mistakes are not comparable. A third entry is untidy and
		// recoverable; overwriting the wrong one destroys a password that may
		// be the one still in use.
		QList<credential> twice;
		twice << credential{ "Old", "alice", QString(), "abc123" }
		      << credential{ "New", "alice", QString(), "def456" };
		check(keepass_protocol::uuid_for_login(twice, "alice").isEmpty(),
		      "two entries sharing a login add rather than overwrite one");

		// A stored entry with no uuid cannot be updated, so it is skipped
		// rather than counted as one of a clashing pair -- otherwise a
		// uuid-less entry sharing the login would suppress a legitimate
		// update and silently add instead.
		//
		// **Both orders, because only one of them discriminates.** With the
		// uuid-less entry first, the running answer is still empty when the
		// real one is reached and the code looks correct with the skip
		// removed; it is the other order that goes wrong. A fixture that
		// covered one order would have tested nothing -- measured, by
		// deleting the skip and watching this section stay green.
		QList<credential> none_first;
		none_first << credential{ "No id", "alice", QString(), QString() }
		           << credential{ "Work", "alice", QString(), "abc123" };
		check(keepass_protocol::uuid_for_login(none_first, "alice") == "abc123",
		      "an entry with no uuid does not hide the one that has one");
		QList<credential> none_last;
		none_last << credential{ "Work", "alice", QString(), "abc123" }
		          << credential{ "No id", "alice", QString(), QString() };
		check(keepass_protocol::uuid_for_login(none_last, "alice") == "abc123",
		      "and it does not when it comes second either");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
