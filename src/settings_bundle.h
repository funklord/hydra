// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

class policy_engine;
class filter_list;

// All of Hydra's settings in one file, so they can be carried to another
// machine, kept in version control, or read without the program.
//
// **The file is an INI**, because everything in it is either a value or a list
// of flat records and neither needs anything cleverer. A key=value file can be
// read by a person, diffed by a tool and edited in an emergency, which is worth
// more here than the ability to nest -- and where the project does use JSON, it
// is for data that is genuinely shaped, not out of habit.
//
//   [hydra]      the format version, so a future reader knows what it has
//   [defaults]   one line per feature: javascript=allow
//   [sites]      one line per exception: news.example=javascript:block, ads:allow
//                (a `*` in a pattern is written `%2A`, because that is how an
//                INI key escapes one; it reads back as the `*` it was)
//   [filters]    the accepted filter rules, numbered
//   [preferences] everything the settings dialog stores, verbatim
//
// **What it does not carry, and why**, since a backup that quietly omits things
// is worse than one that says what it is:
//
//   * The tab tree. That is the session, not a setting, and it has its own file
//     in the same readable spirit.
//   * The Claude API key, which is never written to disk at all.
//   * Learned site rules -- consent wording and detector names. Those have their
//     own import on the Filters page, and it exists because rules from
//     elsewhere are reviewed before they take effect (`site_rules::judge_import`
//     deliberately adds nothing on its own). Carrying them in a one-click
//     restore would route around that review, which is the one thing in this
//     file that is a security property rather than a convenience.
namespace settings_bundle {

// What a write or read did. `error` empty means it worked.
struct summary {
	int     defaults    = 0;   // global feature defaults
	int     sites       = 0;   // per-site exceptions
	int     filters     = 0;   // filter rules
	int     preferences = 0;   // dialog settings keys
	QString error;

	bool    ok() const { return error.isEmpty(); }
	QString describe() const;
};

// The format this build writes. Read refuses anything newer, because a file
// from a later version may mean things this one would misapply.
int current_format();

summary write(const QString &path, const policy_engine *policy,
               const filter_list *filters);

// Merges rather than replaces: defaults and preferences are single-valued and
// are overwritten, while site exceptions and filter rules are added to what is
// already here. Restoring a backup should not silently discard a rule made
// since it was taken.
summary read(const QString &path, policy_engine *policy, filter_list *filters);

}  // namespace settings_bundle
