// Per-node suspended state (architecture doc sec 4.2/sec 5.4).
//
// A blob here is a tab's navigation history: where it had been, what the back
// button would do, where it was scrolled to. Losing one costs a suspended tab
// its past; handing the *wrong* one back is worse, because the tab then claims a
// history that belongs to a different page.
#include "state_store.h"
#include "tab_history.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

	const QString dir = QDir::tempPath() + "/hydra-state-test";
	QDir(dir).removeRecursively();

	section("the ordinary round trip");
	{
		state_store s(dir);
		check(QDir(dir).exists(), "the directory is made rather than assumed");
		check(!s.has_state("a1"), "an unknown id has no state");
		check(s.load("a1").isEmpty(), "and loading it gives nothing, not a crash");

		const QByteArray blob = QByteArray("history-of-a1");
		check(s.save("a1", blob), "a blob saves");
		check(s.has_state("a1"), "and is then known");
		check(s.load("a1") == blob, "and comes back byte for byte");

		check(s.save("a1", "replaced"), "saving again replaces");
		check(s.load("a1") == QByteArray("replaced"),
		      QString("with no tail of the longer one left behind (%1)")
		          .arg(QString::fromUtf8(s.load("a1"))));

		check(s.remove("a1"), "removing says it did");
		check(!s.has_state("a1"), "and it is gone");
		check(!s.remove("a1"), "removing what is not there says so");
	}

	section("blobs are bytes, not text");
	{
		state_store s(dir);
		// A serialized history is binary and contains zero bytes. Anything that
		// treats it as a string truncates at the first one, and the tab comes
		// back with a history that ends where a NUL happened to fall.
		QByteArray binary;
		binary.append('\x00');
		binary.append("middle", 6);
		binary.append('\x00');
		binary.append("\xff\xfe", 2);
		check(s.save("bin", binary), "binary saves");
		check(s.load("bin") == binary,
		      QString("and returns identical, nulls and all (%1 bytes, wanted %2)")
		          .arg(s.load("bin").size())
		          .arg(binary.size()));
		check(s.save("empty", QByteArray()), "an empty blob saves");
		check(s.has_state("empty"),
		      "and counts as present — 'saved nothing' is not 'never saved'");
	}

	section("ids that are not tidy");
	{
		state_store s(dir);
		// Ids come from the tree file, which is documented as human-editable, so
		// they are not guaranteed to be the tidy tokens the app generates.
		check(s.save("a/../../escape", "x"), "an id with path steps in it saves");
		check(s.load("a/../../escape") == QByteArray("x"), "and reads back");
		check(!QDir(dir + "/../..").exists("escape.blob"),
		      "and did not write outside the store");

		check(s.save("with space", "y") && s.load("with space") == QByteArray("y"),
		      "an id with a space works too");
	}

	section("two ids must not share one blob");
	{
		state_store s(dir);
		// **The case worth having a test for.** Sanitising by replacing every
		// unsafe character with '_' is not injective: "a b" and "a_b" both
		// become "a_b". Two suspended tabs would then share one file, and the
		// second to be restored would come back with the first one's history --
		// a tab claiming a past that is not its own.
		check(s.save("a b", "history-of-a-space-b"), "the first id saves");
		check(s.save("a_b", "history-of-a-underscore-b"), "the second saves");
		check(s.load("a b") == QByteArray("history-of-a-space-b"),
		      QString("and the first still has its own history (%1)")
		          .arg(QString::fromUtf8(s.load("a b"))));
		check(s.load("a_b") == QByteArray("history-of-a-underscore-b"),
		      QString("and the second has its own (%1)")
		          .arg(QString::fromUtf8(s.load("a_b"))));
		check(s.remove("a b") && s.has_state("a_b"),
		      "removing one does not remove the other");
	}

	section("blobs written before the collision fix are still found");
	{
		// The fix appends a hash to the filename, but **only when sanitising
		// changed the id** -- so every id the application generates keeps the name
		// it already had on disk. Without that, the fix would have silently
		// orphaned every suspended tab's history on upgrade: no error, no
		// warning, just tabs that had forgotten where they had been.
		const QString old_dir = dir + "/legacy";
		QDir().mkpath(old_dir);
		QFile f(old_dir + "/a1.blob");
		f.open(QIODevice::WriteOnly);
		f.write("history-written-by-the-old-code");
		f.close();

		state_store s(old_dir);
		check(s.has_state("a1"), "an id that needed no sanitising finds its old file");
		check(s.load("a1") == QByteArray("history-written-by-the-old-code"),
		      "and reads what was there");
	}

	section("a second store on the same directory sees the same blobs");
	{
		state_store first(dir);
		first.save("shared", "kept");
		state_store second(dir);
		check(second.load("shared") == QByteArray("kept"),
		      "state outlives the object that wrote it, which is the point of it");
	}

	section("the imported history, which is a record rather than engine state");
	{
		state_store s(dir);
		tab_history h;
		h.entries << history_entry{ "https://one.test/a", "First" }
		           << history_entry{ "https://one.test/b", "Second | with a bar" }
		           << history_entry{ "https://one.test/c", "Third" };
		h.index = 1;

		check(!s.has_history("h1"), "a node with no record has none");
		s.save_history("h1", tab_history_codec::encode(h));
		check(s.has_history("h1"), "and has one once it is written");

		const tab_history back = tab_history_codec::decode(s.load_history("h1"));
		check(back.entries.size() == 3,
		      QString("every entry comes back (%1)").arg(back.entries.size()));
		check(back.index == 1,
		      QString("and the position with them (%1)").arg(back.index));
		check(back.entries.size() == 3 &&
		          back.entries.at(0).url == "https://one.test/a" &&
		          back.entries.at(2).url == "https://one.test/c",
		      "in the order they were visited");
		// The separator is ` | ` and a *title* may contain one. Splitting on
		// the last, or on every, occurrence would truncate this title -- urls
		// cannot contain an unencoded space, so the first is the only one that
		// divides the two fields.
		check(back.entries.size() == 3 &&
		          back.entries.at(1).title == "Second | with a bar",
		      "a title containing the separator survives it");

		// **It shares the id and not the file.** The two have different
		// lifetimes: an engine blob is discarded when the engine moves on and
		// the record must outlive it.
		s.save("h1", "engine bytes, unreadable to anyone else");
		check(s.load("h1") == QByteArray("engine bytes, unreadable to anyone else") &&
		          tab_history_codec::decode(s.load_history("h1")).entries.size() == 3,
		      "a blob and a record under one id do not overwrite each other");

		section("deleting a node takes both, or the next id inherits a past");
		s.remove("h1");
		check(!s.has_state("h1"), "the blob is gone");
		check(!s.has_history("h1"),
		      "and so is the record -- a survivor here is the collision this "
		      "store already exists to prevent, one file over");

		section("what a person may have done to the file by hand");
		// It is a text format on purpose, so it will be edited, and every one
		// of these is a plausible edit rather than a hypothetical.
		check(tab_history_codec::decode("").entries.isEmpty(),
		      "an empty file is an empty record, not a crash");
		check(tab_history_codec::decode("just some other file\n").entries.isEmpty(),
		      "a file that is not this format is refused rather than guessed at");
		const tab_history over = tab_history_codec::decode(
		  "hydra-history 1 | index=9\nhttps://x.test/ | X\n");
		check(over.entries.size() == 1 && over.index == 0,
		      QString("a position past the end is pulled back into the list (%1)")
		          .arg(over.index));
		const tab_history none = tab_history_codec::decode(
		  "hydra-history 1 | index=-1\nhttps://x.test/ | X\n");
		check(none.index == -1,
		      "and an unknown position stays unknown, rather than becoming 0");
		check(none.back_count() == 0 && none.forward_count() == 0,
		      "which counts as nothing behind and nothing ahead");
		// An empty record must not leave a file claiming otherwise.
		s.save_history("h2", tab_history_codec::encode(tab_history{}));
		check(!s.has_history("h2"),
		      "writing an empty record leaves no file to find");
	}

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
