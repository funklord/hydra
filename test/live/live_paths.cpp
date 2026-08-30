//
// Where a live driver's browsing profile lands, decided before anything can
// ask where it is.
//
// **The drivers had quietly become persistent browsers.**
// `qtwebengine_factory` builds a named profile -- `QWebEngineProfile("hydra")`
// -- so that a login survives a restart of the app. Qt puts a named profile
// under `AppDataLocation/QtWebEngine/<name>`, and on this platform
// `AppDataLocation` is `~/.local/share/<applicationName>`, which defaults to
// the executable's own basename. One run of `try_chrome` therefore left 52
// paths behind, 33 of them files: History, Favicons, Visited Links, Local
// Storage, Session Storage and Trust Tokens under
// `~/.local/share/try_chrome/QtWebEngine/hydra`, and the disk cache under
// `~/.cache/try_chrome/QtWebEngine/hydra/Cache`. Nothing anywhere removes
// them, and the profile is built with `AllowPersistentCookies`, so a driver
// carries last week's cookie into this week's run. That is not the fresh
// browser its checks describe, and the drift is silent: it makes a check pass
// rather than fail.
//
// **Three of them were worse.** `try_settings`, `try_watch` and
// `try_downloads` call `setApplicationName("Hydra")`, copied from `main()`'s
// preamble along with the scheme registration and the shared-context
// attribute. That points the same storage at the *user's real profile*, so
// running one while Hydra is open puts two live `QWebEngineProfile`s on one
// directory -- over a leveldb lock and a SQLite cookie database that each
// expect a single writer -- and leaves torrent resume data in the user's own
// `~/.local/share/Hydra/torrents` besides.
//
// `setTestModeEnabled` moves every writable standard location under
// `~/.qttest`: `~/.qttest/share/<applicationName>` for application data,
// `~/.qttest/cache/<applicationName>` for the cache, `~/.qttest/config` for
// settings. The drivers keep the persistence some of them check, and keep it
// somewhere disposable that no shipping program reads. It is deliberately not
// deleted here: an `rm -rf` of a computed path in something every driver links
// is the shape that has eaten a source tree before, and `~/.qttest` is one
// well-known directory a person can clear when they want to.
//
// Two things it does not move, both correct and both worth knowing:
// `DownloadLocation` still answers `~/Downloads`, since it names a place the
// person chose rather than a place the toolkit allocated; and the icon search
// keeps `/usr/share/icons` and the rest of `XDG_DATA_DIRS`, so a driver still
// photographs the desktop's icon theme rather than Qt's fallbacks -- only
// `~/.local/share/icons` drops out of the list.
//
// **A translation unit rather than a line in each of the thirty-seven
// `main()`s**, and the reason is that the failure is silent. A driver that
// forgets the call writes into the real profile and reports nothing; the
// thirty-eighth driver, written next month, would have to remember a rule
// nothing enforces. This file is linked into every live driver by
// `test/Makefile` and into nothing else, so a new driver is covered by
// existing, and the shipping app -- which must keep the real paths -- cannot
// reach it.
//
// The ordering the drivers need is what a namespace-scope object gives for
// free: dynamic initialisation happens before `main()`, so it is already done
// whether the factory is a local in `main()` or a member built in
// `shell::fixture`'s member-initialiser list, where no statement of the
// driver's own could be sequenced ahead of it.
#include <QStandardPaths>

namespace {

// A constructor rather than a `const bool` with a lambda initialiser: a class
// type with a non-trivial constructor is not something -Wunused-variable
// complains about, and the intent reads at the point of definition.
struct test_paths {
	test_paths() { QStandardPaths::setTestModeEnabled(true); }
};

const test_paths g_test_paths;

}  // namespace
