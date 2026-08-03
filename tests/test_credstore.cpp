// Where the KeePassXC pairing lives between runs (architecture doc §13.1, §14).
//
// Two halves, and they need different treatment. The **encoding** is a pure
// function and is tested exhaustively with no keyring anywhere near it — that
// is the part that can be wrong quietly, because a blob that decodes to the
// wrong id produces a pairing KeePassXC refuses and an error message about
// association rather than about storage.
//
// The **storage** needs a real Secret Service, so it runs only where one
// answered and says so where one did not. What it must never do is write to the
// item the user's own pairing lives in: every call here addresses one item by
// fixed attributes, so a suite that saved and cleared under the real name would
// delete a real pairing on exactly the machines where the feature works. The
// store reads `HYDRA_SECRET_KIND` for that reason and this suite refuses to
// touch the service unless it is set to something else.
#include "credential_store.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }

// A pairing round-trips iff decode(encode(x)) == x, so the check is written
// once and pointed at every shape an id can take.
static void round_trips(const QString &id, const QString &key, const char *what) {
	const QString blob = credential_store::encode_pair(id, key);
	QString got_id, got_key;
	const bool ok = credential_store::decode_pair(blob, &got_id, &got_key);
	check(ok && got_id == id && got_key == key,
	      QString("%1 survives the round trip (%2)")
	          .arg(what, ok ? got_id : QStringLiteral("did not decode")));
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("the encoding, which needs no keyring");
	// A KeePassXC association id is whatever the user typed into the dialog, so
	// the interesting cases are the ones a person really produces.
	round_trips("hydra", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", "a plain name");
	round_trips("my laptop", "a2V5", "a name with a space");
	round_trips("nabbe's browser", "a2V5", "a name with an apostrophe");
	round_trips("hydra\non two lines", "a2V5", "a name with a newline");
	round_trips("køyretøy — nettlesar", "a2V5", "a name outside ASCII");
	round_trips(QString(300, QChar('x')), "a2V5", "a very long name");
	// The separator itself, which is the one character the format spends.
	round_trips("a b c d", "a2V5", "a name that is mostly separators");

	section("what is not a pairing");
	// Encoding refuses a half. Storing one would produce a stored pairing that
	// can never be used and can only be discovered at the next connect.
	check(credential_store::encode_pair("", "a2V5").isEmpty(),
	      "no id, no blob");
	check(credential_store::encode_pair("hydra", "").isEmpty(),
	      "no key, no blob");
	check(credential_store::encode_pair("", "").isEmpty(),
	      "neither, no blob");

	QString id, key;
	check(!credential_store::decode_pair("", &id, &key),
	      "an empty blob does not decode");
	check(!credential_store::decode_pair("no-separator-here", &id, &key),
	      "a blob with no separator does not decode");
	check(!credential_store::decode_pair(" a2V5", &id, &key),
	      "a blob with no id half does not decode");
	check(!credential_store::decode_pair("aHlkcmE=", &id, &key),
	      "an id with no key half does not decode");
	check(!credential_store::decode_pair("aHlkcmE= ", &id, &key),
	      "a trailing separator is not a key");
	check(!credential_store::decode_pair("!!!! a2V5", &id, &key),
	      "an id half that is not base64 does not decode");

	// The one that matters most: a decode that fails must not leave the caller
	// holding a plausible-looking half. The bridge passes these straight to
	// set_association, so a partial write here becomes a pairing on the wire.
	{
		QString before_id = "untouched", before_key = "untouched";
		credential_store::decode_pair("garbage", &before_id, &before_key);
		check(before_id == "untouched" && before_key == "untouched",
		      "a failed decode writes neither half");
	}
	// And decode must tolerate being asked for nothing, since has_pairing()
	// only wants the verdict.
	check(!credential_store::decode_pair("garbage", nullptr, nullptr),
	      "a verdict-only decode does not crash on null");
	check(credential_store::decode_pair("aHlkcmE= a2V5", nullptr, nullptr),
	      "and answers yes for a good blob");

	section("the availability contract");
	// The documented invariant, and the one the UI leans on: exactly one of
	// these is true, so a caller can print the reason whenever the feature is
	// off and never print an empty string.
	check(credential_store::available() ==
	          credential_store::unavailable_reason().isEmpty(),
	      QString("available() and a reason are exact opposites (%1)")
	          .arg(credential_store::available()
	                   ? QStringLiteral("available")
	                   : credential_store::unavailable_reason()));

	section("storing it, against a real Secret Service");
	const QByteArray kind = qgetenv("HYDRA_SECRET_KIND");
	if (!credential_store::available()) {
		note("skipped: " + credential_store::unavailable_reason());
		note("The encoding above is checked either way; what is unrun is the");
		note("service round trip, which needs a keyring to be running.");
	} else if (kind.isEmpty() || kind == "keepassxc-association") {
		note("skipped: HYDRA_SECRET_KIND is not set to a test-only value.");
		note("This suite writes and deletes one keyring item, and under the");
		note("default name that item is the user's real pairing. Run it as:");
		note("  HYDRA_SECRET_KIND=hydra-test-$$ ./tests/build/test_credstore");
		note("Refusing rather than skipping quietly, because a suite that");
		note("deleted a real pairing would look exactly like one that passed.");
	} else {
		// Start from nothing, whatever a previous run left.
		credential_store::clear();
		check(!credential_store::has_pairing(),
		      "nothing is stored before the first save");
		check(!credential_store::load(&id, &key),
		      "and loading it fails rather than returning stale halves");

		const QString the_id  = "hydra test pairing";
		const QString the_key = "c2VjcmV0LWtleS1tYXRlcmlhbA==";
		check(credential_store::save(the_id, the_key), "a pairing saves");
		check(credential_store::has_pairing(), "and is then present");

		// The load is done *before* the message is built, and that is not
		// stylistic. C++ does not order the evaluation of a call's arguments,
		// so `check(load(&id), QString(...).arg(id))` may format the id before
		// load() has written it -- which it did here, printing an empty name
		// beside a passing check and, on the next one, the *previous* pairing's
		// name. The assertion was right and its evidence was not, which is the
		// failure this project keeps writing down: report what was seen.
		QString back_id, back_key;
		bool got = credential_store::load(&back_id, &back_key);
		check(got && back_id == the_id && back_key == the_key,
		      QString("and comes back byte for byte (%1)")
		          .arg(got ? back_id : QStringLiteral("did not load")));

		// Overwriting rather than accumulating: re-pairing must replace, or the
		// second lookup finds whichever item the service happens to return.
		const QString new_id = "hydra test pairing, again";
		check(credential_store::save(new_id, "b3RoZXIta2V5"), "a second save works");
		got = credential_store::load(&back_id, &back_key);
		check(got && back_id == new_id && back_key == "b3RoZXIta2V5",
		      QString("and replaces the first rather than adding to it (%1)")
		          .arg(got ? back_id : QStringLiteral("did not load")));

		check(credential_store::clear(), "clearing works");
		check(!credential_store::has_pairing(), "and then nothing is stored");
		check(!credential_store::load(&back_id, &back_key),
		      "and a load after clearing fails rather than returning the old one");

		// Leave the keyring as it was found. A test item that outlives the run
		// is litter in the user's keyring, and one named like ours is worse.
		credential_store::clear();
		note("keyring item removed; nothing left behind");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
