#pragma once

#include "media_evidence.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class ai_provider;

// **What a video is, as a weak model reads the evidence, checked.** The
// second of the three steps agreed on 2026-10-10: `media_evidence` gathers,
// this interprets, the person corrects.
//
// The model is asked the questions rules could not answer -- the holder's
// examples were whether the artist is the uploader or is in the title, and
// whether a long upload is a set -- and answers in a fixed shape. **It may
// choose and split, but not invent**: every name it gives must appear in the
// evidence, and the tracklist it points at must be one the evidence holds,
// in order and within the video's length. A part that fails its check is
// replaced by the rules' answer and marked as a fallback, and the failure is
// counted against that task, which is how the browser can say which tasks a
// model is too weak for rather than leaving the person to notice.
struct media_reading {
	// The agreed categories. A wide spread of small kinds goes to misc.
	enum class kind { music, movie, episode, talk, learning, news, gaming,
	                  short_clip, misc };
	kind    what = kind::misc;
	QString artist;          // who it is by, as the name to file it under
	QString title;           // what it is called, as the track
	bool    is_set = false;  // a mix, an album, a compilation
	QString tracklist;       // a `media_evidence::tracklist::source`, or empty
	double  confidence = 0;  // the model's own, 0 to 1

	// The tasks, each judged on its own: "kind", "artist", "title",
	// "tracklist". Those listed here were answered by the model and kept;
	// the rest are the rules' answer.
	QStringList from_model;
	// Why each rejected task was rejected, "artist: ..." -- shown to the
	// person and counted against the model.
	QStringList rejected;
	// **Kept, but suspect**: "title: ...". A check that cannot reject --
	// the string is in the metadata, so grounding passes -- and can still
	// see something wrong with it: the artist's name, or a tag like
	// "(Official Audio)", left in the title. Measured on the corpus, that is
	// exactly what the 3B models get wrong, and without this the tally
	// counted every such title as kept, so a person running one was told
	// nothing about its worst task.
	QStringList flagged;
	bool        model_answered = false;   // false: rules throughout

	// Whether the model's answer for `task` was kept and not flagged --
	// what the tally counts as a good answer.
	bool clean(const QString &task) const;

	static QString name_of(kind k);
	static bool    kind_from(const QString &name, kind *out);
	static QStringList kind_names();
};

class media_interpretation : public QObject {
	Q_OBJECT
public:
	media_interpretation(ai_provider *provider, const media_evidence &evidence,
	                     QObject *parent = nullptr);
	~media_interpretation() override;

	// Asks the model, or answers by rules at once when there is none.
	void start();

	// --- pure, so each half can be tested without a model -----------------
	static QString system_prompt();
	static QString prompt_for(const media_evidence &ev);
	// The rules' reading: what is used when there is no model, and for each
	// task a model's answer to fails its check.
	static media_reading by_rules(const media_evidence &ev);
	// The model's reply, checked task by task against the evidence, with
	// the rules filling in what failed.
	static media_reading checked(const QString &reply, const media_evidence &ev);
	// Does `value` appear in the evidence a name may be copied from? Case
	// and runs of whitespace aside.
	static bool grounded(const QString &value, const media_evidence &ev);
	// What looks wrong with a title, without deciding it is: the artist's
	// name in it as a whole word (a self-titled song aside), or a tag that
	// names the upload rather than the work, bracketed or at the end.
	static QStringList title_flags(const QString &title, const QString &artist);

signals:
	void finished(const media_reading &reading);

private:
	void on_reply(const QString &reply);
	void on_failed(const QString &error);

	QPointer<ai_provider> m_provider;
	media_evidence        m_evidence;
	bool                  m_waiting = false;
};
