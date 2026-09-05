#pragma once

#include "local_proxy.h"
#include "media_detector.h"

#include <QObject>
#include <QString>
#include <QTemporaryDir>

class download_manager;
class hls_assembler;
class player_launcher;

// Assembling an HLS stream into one local file, and everything that has to
// still be true afterwards (architecture doc sec 11.2/sec 11.3).
//
// **This lived in `media_dialog`, and the dialog is the wrong lifetime for
// it.** That dialog is opened with `exec()` from a stack object, so closing it
// destroyed the assembler -- ending the assembly part-way -- and destroyed the
// `QTemporaryDir` the assembled bytes were being written into, which removed
// the file while an external player was reading it. The tee-to-disk trick
// sec 11.3 describes hands a player a file that is still growing and then says
// "playback continues locally"; that sentence was true only for as long as the
// picker stayed open, which is the opposite of what a picker is for.
//
// So the scratch directory and the assembler belong to the window instead, and
// the dialog drives this rather than owning it -- the same shape as the player
// launcher, the download manager and the proxy, all of which the dialog is
// handed and none of which it owns.
//
// The status line is a signal for the same reason. While the dialog is open it
// wants the progress in its own label; once it has gone the window's status bar
// is the only place left to say that an assembly is still running, and an
// assembly nobody can see is one people kill by quitting.
class stream_assembly : public QObject {
	Q_OBJECT
public:
	stream_assembly(player_launcher *players, download_manager *downloads,
	                 local_proxy *proxy, QObject *parent = nullptr);

	// Assemble, then hand the growing file to the external player -- the case
	// for a player that cannot take a manifest itself.
	void watch(const media_item &item, const stream_context &ctx);
	// Assemble into the download directory and rewrap afterwards if ffmpeg is
	// there. The output survives either way; only the container is optional.
	void save(const media_item &item, const stream_context &ctx);

	// Whether an assembly is running, and where it is writing. Both are here
	// for the window and for tests: the file's survival is the property this
	// class exists for, and a test that cannot name the file cannot check it.
	bool    running() const;
	QString output_path() const;
	// Empty when the temporary directory could not be created, which is a
	// state `watch` reports rather than one it papers over.
	QString scratch_path() const;

signals:
	// Plain text, deliberately: it goes to a QLabel and to a status bar, and
	// only one of those renders markup.
	void status(const QString &text);

private:
	void assemble(const media_item &item, const stream_context &ctx,
	               bool play_it);

	player_launcher  *m_players   = nullptr;   // injected, not owned
	download_manager *m_downloads = nullptr;   // injected, not owned
	local_proxy      *m_proxy     = nullptr;   // injected, not owned
	hls_assembler    *m_assembler = nullptr;
	QTemporaryDir     m_scratch;
	// One name per assembly. A fixed one meant watching a second stream
	// truncated the file the first player was still reading.
	int               m_stream_seq = 0;
};
