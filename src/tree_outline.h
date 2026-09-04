#pragma once

#include "node.h"   // for node_type in the vocabulary helpers below

#include <QString>

// Reads and writes the canonical, human-readable tree file -- the source of
// truth for structure and order (architecture doc sec 4.4). Format, two spaces
// per depth level:
//
//   - [f0] folder | Work
//     - [a2] open | Jira board | https://jira.example.com/board | created=... | seen=...
//     - [a4] unopened | Recipe | https://cook.example.com/stew
//
// Fields are separated by " | ". The first field is the node type; trailing
// key=value fields (created=, seen=) are optional.
namespace tree_outline {

// Returns a synthetic root node (owns the whole tree; delete it to free).
// An absent file gives an empty root, which is an ordinary first run.
//
// **A file that is there and yields nothing gives NOTHING**, and the
// difference is the whole point: an empty tree is what a first run
// legitimately produces, so a caller handed one cannot tell a fresh install
// from a tree it has just failed to read -- and the next save writes that
// empty tree back over the file. Returning null makes the caller stop.
//
// Anything nested deeper than `tree_limits::max_depth` is **flattened to that
// depth** rather than refused: refusing a file loses tabs, flattening loses
// only nesting. `flattened` receives how many nodes were moved up, so the
// caller can say so -- a tree that quietly changed shape on load is the kind
// of thing somebody discovers much later and cannot explain.
//
// `unparsed` is the same promise for the other way a line is lost: content
// that is not blank and is not a node, which the loop skips. **Skipping is
// right and silence is not** -- the next save writes back what parsed, so a
// line nobody was told about is a line that stops existing.
node *load(const QString &path, int *flattened = nullptr,
            int *unparsed = nullptr);

bool  save(const QString &path, node *root);

// The shared type vocabulary. Exposed because the AI payload (tree_serializer)
// uses the same words -- one spelling of "folder"/"open" across the project.
QString   type_to_string(node_type t);
node_type type_from_string(const QString &s);

}  // namespace tree_outline
