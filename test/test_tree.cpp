// The tree file, which is the user's data (architecture doc sec 4.4).
//
// It is the source of truth for structure and order, it is written on every
// change, and it is read back on every launch -- so a round-trip that loses
// something loses the thing the whole application is about. Nothing tested it.
//
// The same field format is used for the AI reorganizer's payload
// (`tree_serializer`), so both are here: they share a separator and therefore
// share whatever that separator gets wrong.
#include "tree_outline.h"
#include "tree_serializer.h"
#include "node.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdio>
#include <unistd.h>   // geteuid, for the checks root cannot fail


static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static node *child(node *parent, int i) {
	return (parent && i < parent->children.size()) ? parent->children[i] : nullptr;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QString dir = QDir::tempPath() + "/hydra-tree-test";
	QDir().mkpath(dir);
	const QString path = dir + "/tree.txt";

	section("a tree survives being written and read");
	{
		node root;
		root.id = "root";
		root.type = node_type::folder;

		auto *folder = new node;
		folder->id = "f0";
		folder->type = node_type::folder;
		folder->title = "Work";
		folder->parent = &root;
		root.children.push_back(folder);

		auto *page = new node;
		page->id = "a1";
		page->type = node_type::open_tab;
		page->title = "Jira board";
		page->url = "https://jira.example.com/board";
		page->created = QDateTime::fromString("2026-01-02T03:04:05", Qt::ISODate);
		page->last_seen = QDateTime::fromString("2026-02-03T04:05:06", Qt::ISODate);
		page->parent = folder;
		folder->children.push_back(page);

		auto *loose = new node;
		loose->id = "a2";
		loose->type = node_type::unopened_tab;
		loose->title = "Recipe";
		loose->url = "https://cook.example.com/stew";
		loose->parent = &root;
		root.children.push_back(loose);

		check(tree_outline::save(path, &root), "it saves");
		node *back = tree_outline::load(path);
		check(back != nullptr, "and loads");
		if (back) {
			check(back->children.size() == 2,
			      QString("with both top-level nodes (%1)").arg(back->children.size()));
			node *f = child(back, 0);
			check(f && f->title == "Work" && f->is_folder(), "the folder comes back");
			check(f && f->children.size() == 1, "with its child inside it");
			node *p = child(f, 0);
			check(p && p->title == "Jira board", "the page's title");
			check(p && p->url == "https://jira.example.com/board", "and its url");
			check(p && p->type == node_type::open_tab, "and its state");
			check(p && p->created.toString(Qt::ISODate) == "2026-01-02T03:04:05",
			      "created survives");
			check(p && p->last_seen.toString(Qt::ISODate) == "2026-02-03T04:05:06",
			      "and last seen");
			check(child(back, 1) && child(back, 1)->title == "Recipe",
			      "and order is kept, not sorted");
			delete back;
		}
	}

	section("a title with the field separator in it");
	{
		// **This is the case that matters**, and it is not exotic: "Article Title
		// | Site Name" is one of the commonest shapes a page title takes on the
		// web. The file's fields are separated by " | ", so a title containing
		// one used to shift every field after it -- the url became the tail of the
		// title, and the real url was left in a position nothing reads.
		node root;
		root.id = "root";
		root.type = node_type::folder;

		auto *page = new node;
		page->id = "a1";
		page->type = node_type::unopened_tab;
		page->title = "Some Article | The Daily Example";
		page->url = "https://daily.example/article";
		page->parent = &root;
		root.children.push_back(page);

		check(tree_outline::save(path, &root), "it saves");
		node *back = tree_outline::load(path);
		node *p = child(back, 0);
		check(p != nullptr, "and the node comes back");
		check(p && p->title == "Some Article | The Daily Example",
		      QString("the whole title, separator and all (%1)")
		          .arg(p ? p->title : QString()));
		check(p && p->url == "https://daily.example/article",
		      QString("and the url is the url, not the rest of the title (%1)")
		          .arg(p ? p->url : QString()));
		delete back;
	}

	section("a folder title with a separator, which has no url after it");
	{
		node root;
		root.id = "root";
		root.type = node_type::folder;
		auto *f = new node;
		f->id = "f9";
		f->type = node_type::folder;
		f->title = "Recipes | Winter";
		f->parent = &root;
		root.children.push_back(f);

		tree_outline::save(path, &root);
		node *back = tree_outline::load(path);
		node *g = child(back, 0);
		check(g && g->title == "Recipes | Winter",
		      QString("a folder keeps its whole title too (%1)")
		          .arg(g ? g->title : QString()));
		check(g && g->url.isEmpty(), "and gains no url from it");
		delete back;
	}

	section("nesting and order");
	{
		const QByteArray text =
		  "- [f0] folder | A\n"
		  "  - [f1] folder | B\n"
		  "    - [a1] unopened | deep | https://x.example/1\n"
		  "  - [a2] unopened | shallow again | https://x.example/2\n"
		  "- [f2] folder | C\n";
		QFile f(path);
		f.open(QIODevice::WriteOnly | QIODevice::Truncate);
		f.write(text);
		f.close();

		node *back = tree_outline::load(path);
		check(back && back->children.size() == 2, "two top-level folders");
		node *a = child(back, 0);
		check(a && a->children.size() == 2,
		      QString("A has two children (%1)").arg(a ? a->children.size() : -1));
		node *b = child(a, 0);
		check(b && b->children.size() == 1, "B has the deep one");
		check(child(b, 0) && child(b, 0)->title == "deep", "which is the deep one");
		check(child(a, 1) && child(a, 1)->title == "shallow again",
		      "and dedenting puts the next node back under A");
		delete back;
	}

	section("the AI payload round-trips the same way");
	{
		node root;
		root.id = "root";
		root.type = node_type::folder;
		auto *p = new node;
		p->id = "a1";
		p->type = node_type::unopened_tab;
		p->title = "Some Article | The Daily Example";
		p->url = "https://daily.example/article";
		p->tags = QStringList{"news", "later"};
		p->parent = &root;
		root.children.push_back(p);

		const QString payload = tree_serializer::to_payload(&root);
		node *back = tree_serializer::parse_proposal(payload);
		check(back != nullptr, "a payload it wrote is a payload it can read");
		node *q = child(back, 0);
		check(q && q->title == "Some Article | The Daily Example",
		      QString("with the title intact (%1)").arg(q ? q->title : QString()));
		check(q && q->url == "https://daily.example/article",
		      QString("and the url (%1)").arg(q ? q->url : QString()));
		check(q && q->tags == QStringList({"news", "later"}),
		      "and the tags, which live after the url");
		delete back;
	}

	section("a proposal wrapped in prose, as a model actually answers");
	{
		const QString reply =
		  "Sure! Here is the reorganised tree:\n"
		  "```\n"
		  "- [f0] folder | Reading\n"
		  "  - [a1] unopened | A page | https://x.example/\n"
		  "```\n"
		  "Let me know if you would like it different.\n";
		node *back = tree_serializer::parse_proposal(reply);
		check(back && back->children.size() == 1,
		      "the outline is taken and the prose ignored");
		check(child(back, 0) && child(back, 0)->children.size() == 1,
		      "including the nesting inside the fence");
		delete back;
		check(tree_serializer::parse_proposal("I cannot do that.") == nullptr,
		      "and an answer with no outline at all is refused, not empty");
	}

	section("saving is atomic, so an interrupted browser keeps its tree");
	{
		// The tree is written on a debounce timer while the browser runs, not
		// only on the way out, so the window between emptying the file and
		// finishing the last line is a window the process can die in. What
		// this suite can establish is the mechanism and its housekeeping --
		// that the write goes through a temporary and a rename, that it
		// reports failure, and that it leaves nothing behind. That the rename
		// itself is atomic is rename(2)'s guarantee and not something a test
		// here can add to.
		const QString adir = dir + "/atomic";
		QDir(adir).removeRecursively();
		QDir().mkpath(adir);
		const QString apath = adir + "/tree.txt";

		node root;
		root.id   = "root";
		root.type = node_type::folder;
		auto *page  = new node;
		page->id    = "a1";
		page->type  = node_type::unopened_tab;
		page->title = "A page";
		page->url   = "https://x.example/";
		page->parent = &root;
		root.children.push_back(page);

		check(tree_outline::save(apath, &root), "a save reports success");
		check(QDir(adir).entryList(QDir::Files | QDir::Hidden).size() == 1,
		      "and leaves exactly one file: no temporary is left beside it");

		// The atomic write creates a new inode, so the mode has to be carried
		// across it or a rewrite quietly republishes a private tree at
		// whatever the umask says -- 0644 here, which is the whole of
		// somebody's browsing on a shared machine.
		//
		// **Read the permissions back rather than comparing against the flags
		// that were set.** On Unix `QFile::permissions()` reports the Owner
		// bits *and* the User bits for the same three permissions, so setting
		// `ReadOwner | WriteOwner` reads back as four flags, not two. The
		// first version of this check compared against the two and failed
		// against code that was correct; measuring what QSaveFile actually
		// left on disk (`-rw-------`) is what settled it.
		QFile::setPermissions(apath, QFile::ReadOwner | QFile::WriteOwner);
		const QFileDevice::Permissions strict = QFile::permissions(apath);
		check(!strict.testFlag(QFile::ReadGroup) &&
		        !strict.testFlag(QFile::ReadOther),
		      "a tree file can be made private");
		check(tree_outline::save(apath, &root), "a second save over it succeeds");
		check(QFile::permissions(apath) == strict,
		      "and the atomic write keeps the mode, rather than the umask's");

		// The reason atomicity is worth having: what is on disk after a save
		// that cannot finish must be the tree that was there, never a stump.
		// A read-only target is the failure this suite can produce on demand.
		//
		// **Except as root, for whom the permission bits are advice.** uid 0
		// writes a 0400 file without complaint, so the save succeeds, and the
		// two assertions below then fail against code that is correct. That is
		// what CI reported: the workflow's build job runs in a `debian:trixie`
		// container, which is root, and these were red there while green on
		// every developer machine. Skipped rather than adapted, because the
		// premise genuinely does not hold and a check that cannot fail is
		// worse than one that says it did not run. CI runs the suite as an
		// ordinary user now, so this is the belt to that braces.
		if (::geteuid() == 0) {
			std::printf("  --    running as root: a read-only file is still "
			             "writable, so the failed-save checks are skipped\n");
		} else {
			const QByteArray before = [&] {
				QFile f(apath);
				f.open(QIODevice::ReadOnly);
				return f.readAll();
			}();
			QFile::setPermissions(apath, QFile::ReadOwner);

			auto *second  = new node;
			second->id    = "a2";
			second->type  = node_type::unopened_tab;
			second->title = "Another";
			second->url   = "https://y.example/";
			second->parent = &root;
			root.children.push_back(second);

			check(!tree_outline::save(apath, &root),
			      "a save onto a file it cannot write says so");
			const QByteArray after = [&] {
				QFile f(apath);
				f.open(QIODevice::ReadOnly);
				return f.readAll();
			}();
			check(after == before,
			      "and the tree that was there is still there, whole");
			check(QDir(adir).entryList(QDir::Files | QDir::Hidden).size() == 1,
			      "with no half-written temporary left in the directory");

			QFile::setPermissions(apath, QFile::ReadOwner | QFile::WriteOwner);
		}
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
