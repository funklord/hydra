// What it would cost to checkpoint every live tab's history.
//
// **This driver exists to answer one question project.md left open.** The
// crash story is complete except for the live tabs' navigation history: a
// state blob is written when a view is suspended, so a crash loses the
// history of everything still open. The entry says the fix -- serialising
// every live view on a timer -- is "real work against a loss only a crash
// produces, and it wants a measurement of what that costs on a window full
// of tabs before anybody commits to it."
//
// That measurement needs a real engine: `save_state()` is Qt WebEngine's own
// serialisation and the suite's fake returns an empty QByteArray, so nothing
// offline can say what it costs. Hence a live driver rather than a case in
// `make test`.
//
// It reports rather than judges. The three checks are that the measurement
// happened at all -- a blob that comes back empty would make every number
// below a measurement of nothing, which is the one way this could look like
// an answer and be none.
//
//   QT_QPA_PLATFORM=offscreen ./test/build-make/try_blob_cost
//   HYDRA_BLOB_TABS=16 QT_QPA_PLATFORM=offscreen ./test/build-make/try_blob_cost
//
// **Bounded deliberately**, per the workspace rule about anything that spawns
// processes: each live tab is a renderer, so the tab count is clamped to 32
// and the driver tears the window down itself rather than leaving it to exit.
#include "shell_fixture.h"
#include "node.h"
#include "state_store.h"
#include "tab_tree_model.h"
#include "tree_sort_proxy.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	// **No `theme::apply` here**: `shell::fixture` does it in its own
	// constructor, before the window is built, and its comment records why a
	// driver that applies the scheme itself photographs a browser nobody
	// runs. Doing it twice is not harmful and is one more thing to keep in
	// step with that constructor.

	// Default eight, which is `k_default_live_views` -- the window the
	// question is actually about. Clamped at 32: each one is a renderer
	// process, and this driver is not the place to find out what a hundred do.
	int tabs = 8;
	if (qEnvironmentVariableIsSet("HYDRA_BLOB_TABS"))
		tabs = qBound(1, qEnvironmentVariableIntValue("HYDRA_BLOB_TABS"), 32);
	// The cap has to be at least the tab count or the window suspends the
	// views as fast as they are opened and there is nothing live to measure.
	qputenv("HYDRA_MAX_LIVE_VIEWS", QByteArray::number(tabs + 1));

	shell::fixture f(QStringLiteral("blob-cost"));
	shell::section("a window full of live tabs");
	std::printf("  measuring %d live tab(s)\n", tabs);

	// Each tab visits both pages, so every view has real back/forward history
	// rather than a single entry. A blob for one entry is not the blob a
	// checkpoint would be writing.
	//
	// **Opened the way a person opens a tab**, through the tree's own
	// `activated` signal, which is the discipline `shell_fixture.h` states
	// for every driver here: nothing reaches into private members, because a
	// driver that calls the shell's internals is not driving the shell.
	QList<web_view_backend *> views;
	for (int i = 0; i < tabs; ++i) {
		node *n = f.window.m_model->add_tab(nullptr, QString("tab %1").arg(i),
		                                     QUrl::fromLocalFile(f.one).toString());
		if (!n)
			continue;
		emit f.tv->activated(
		  f.window.m_proxy->mapFromSource(f.window.m_model->index_for_node(n)));
		shell::wait_for(f.address, "one");
		web_view_backend *v = f.window.m_views_by_id.value(n->id, nullptr);
		if (!v)
			continue;
		v->load(QUrl::fromLocalFile(f.two));
		shell::wait_for(f.address, "two");
		views << v;
	}
	shell::check(views.size() == tabs,
	              QString("%1 of %2 tabs are live")
	                  .arg(views.size()).arg(tabs));

	// --- the serialisation half
	QList<qint64> each_us;
	qint64 total_bytes = 0;
	QElapsedTimer t;
	t.start();
	for (web_view_backend *v : views) {
		QElapsedTimer one;
		one.start();
		const QByteArray blob = v->save_state();
		each_us << one.nsecsElapsed() / 1000;
		total_bytes += blob.size();
	}
	const qint64 serialise_us = t.nsecsElapsed() / 1000;

	// **The control.** An empty blob would make every number above a
	// measurement of nothing, and it is exactly what the offline fake
	// returns -- so a driver built against the wrong backend would print a
	// confident zero.
	shell::check(total_bytes > 0,
	              QString("the blobs carry something (%1 bytes in all)")
	                  .arg(total_bytes));

	// --- the disk half, which is the other thing a timer would be doing
	QTemporaryDir scratch;
	qint64 write_us = 0;
	int written = 0;
	if (scratch.isValid()) {
		state_store store(scratch.path());
		QElapsedTimer w;
		w.start();
		for (int i = 0; i < views.size(); ++i)
			if (store.save(QString("m%1").arg(i), views[i]->save_state()))
				++written;
		write_us = w.nsecsElapsed() / 1000;
	}
	shell::check(written == views.size(),
	              QString("every blob reached disk (%1 of %2)")
	                  .arg(written).arg(views.size()));

	std::sort(each_us.begin(), each_us.end());
	const qint64 median = each_us.isEmpty() ? 0 : each_us[each_us.size() / 2];
	const qint64 worst  = each_us.isEmpty() ? 0 : each_us.last();

	std::printf("\n-- what a checkpoint of every live tab costs --\n");
	std::printf("  tabs                     %d\n", int(views.size()));
	std::printf("  save_state, median       %lld us\n", (long long)median);
	std::printf("  save_state, worst        %lld us\n", (long long)worst);
	std::printf("  save_state, all of them  %lld us\n", (long long)serialise_us);
	std::printf("  blob bytes, all of them  %lld\n", (long long)total_bytes);
	std::printf("  bytes per tab, mean      %lld\n",
	             (long long)(views.isEmpty() ? 0 : total_bytes / views.size()));
	std::printf("  write to disk, all       %lld us\n", (long long)write_us);
	std::printf("  serialise + write        %lld us\n",
	             (long long)(serialise_us + write_us));
	std::printf("\n  Read it against the interval a checkpoint would use.\n"
	             "  project.md's open question is whether this is \"real work\";\n"
	             "  these are the numbers it asked for and not an answer to it.\n");

	// Torn down here rather than at exit: each view is a renderer, and a
	// driver that leaves them to the process teardown is the shape the
	// workspace rule about spawning is written against.
	f.window.close();
	shell::spin(200);
	return shell::report();
}
