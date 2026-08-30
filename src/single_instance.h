#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QString>
#include <QStringList>

#include <functional>

// One Hydra per profile directory, and a way for the second one to say what it
// was asked to open.
//
// **This became necessary the moment the web engine profile stopped being
// off the record.** An off-the-record profile allocates its own storage per
// process, so two copies of the browser were independent and nothing bad
// happened. A named profile is a directory on disk holding leveldb databases
// with their own LOCK files and a SQLite `Cookies` database, every one of
// which expects a single writer -- and Chromium does not arbitrate between
// two engines opening the same directory, it corrupts. A user double-clicking
// the launcher reaches that, and so does anybody starting the browser from a
// terminal while a window is already up.
//
// Two mechanisms, because they answer two different questions:
//
//   * **`QLockFile` decides who runs.** Its acquisition is atomic, which is
//     what a socket cannot offer -- see the comment on `acquire()` for the
//     race that rules the socket out as the arbiter.
//   * **`QLocalServer` carries the hand-over**, so the second instance can
//     give the running one the url it was launched with instead of dying with
//     the link in its hand. Hydra installs itself as the default browser
//     (`Exec=hydra %U`), so "a second instance" is most often a link click.
//
// Losing the socket costs the hand-over and nothing else: exclusivity is the
// lock's alone, and a build where the socket cannot be created still refuses
// to open the profile twice.
class single_instance {
public:
	// `dir` is the directory being protected -- Hydra passes its
	// `AppDataLocation`, which holds the tree, the state directory and the web
	// engine profile. Everything is keyed on it rather than on a fixed name so
	// that two runs pointed at different `XDG_DATA_HOME`s are genuinely two
	// different applications, which is what makes an isolated test run
	// possible at all.
	explicit single_instance(const QString &dir);
	~single_instance();

	single_instance(const single_instance &) = delete;
	single_instance &operator=(const single_instance &) = delete;

	// True when this process may open `dir`. False means another one has it,
	// and `owner()` describes that one.
	bool acquire();

	// The refused side: hand `message` to the instance that holds the
	// directory. Returns false when there is nobody to hand it to, which the
	// caller must treat as "do not start anyway".
	bool hand_over(const QString &message);

	// The holding side: what to do with a message a later instance handed
	// over. An empty message means "you were launched again with nothing to
	// open", which is a request to come to the front.
	void on_message(std::function<void(const QString &)> handler);

	// A description of the process holding the directory, for the message the
	// refused instance prints. Empty until `acquire()` has failed.
	QString owner() const { return m_owner; }

private:
	void accept_peer();
	void deliver(const QString &message);

	QString      m_lock_path;
	QString      m_socket_path;
	QLockFile    m_lock;
	QLocalServer *m_server = nullptr;
	// Whether this process is the one holding the directory. The destructor
	// needs it: a refused instance must take nothing away on its way out.
	bool m_primary = false;

	std::function<void(const QString &)> m_handler;
	// Messages that arrived before the window existed. The handler needs the
	// window, and the window is built after the guard for the whole point of
	// the guard -- nothing must construct the profile before we know we may.
	QStringList m_pending;

	QString m_owner;
};
