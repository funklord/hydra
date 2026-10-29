#pragma once

#include <QString>

// **What a page's article is, found by reading its DOM** -- the first half of
// reader mode (`reader_page.h` is the second). Listed as unbuilt in
// project.md's "What is next" since 2026-09-18 and built 2026-10-29, with an
// extractor of this project's own rather than Mozilla's Readability: the
// holder chose that, so no third party's terms come with the feature.
//
// The approach is the long-established one, written from scratch: every
// paragraph scores its parent and grandparent by its length and commas, a
// container's class or id names it as content or as chrome, links thin a
// score out, and the best container -- with whichever of its siblings score
// well enough -- is the article. It runs in the isolated world through
// `web_view_backend::run_probe`, so it reads the page and the page cannot see
// it, and it changes nothing in the page: the answer is rebuilt from an
// allow-list of tags and attributes, never copied as markup, so no script,
// style or handler of the page's can come along.
namespace reader_extract {

struct article {
	QString title;
	QString byline;
	QString site;
	QString lang;
	QString dir;        // "rtl" or "ltr"
	QString html;       // the allow-listed markup of the article
	int     words = 0;
};

// The extractor, ES5. Its answer is a JSON string.
QString source();

// Its answer, read. False with `why` set when the page has no article the
// extractor could find, or the answer cannot be read -- an empty answer is
// what a view that cannot run scripts gives.
bool parse(const QString &json, article *out, QString *why);

}  // namespace reader_extract
