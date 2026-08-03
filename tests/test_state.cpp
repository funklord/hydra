// Per-node suspended state (architecture doc §4.2/§5.4).
//
// A blob here is a tab's navigation history: where it had been, what the back
// button would do, where it was scrolled to. Losing one costs a suspended tab
// its past; handing the *wrong* one back is worse, because the tab then claims a
// history that belongs to a different page.
#include "state_store.h"

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
		// second to be restored would come back with the first one's history —
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
		// changed the id** — so every id the application generates keeps the name
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

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
