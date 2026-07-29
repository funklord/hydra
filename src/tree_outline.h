// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "node.h"   // for node_type in the vocabulary helpers below

#include <QString>

// Reads and writes the canonical, human-readable tree file — the source of
// truth for structure and order (architecture doc §4.4). Format, two spaces
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
// On failure or empty file, returns an empty root.
node *load(const QString &path);

bool  save(const QString &path, node *root);

// The shared type vocabulary. Exposed because the AI payload (tree_serializer)
// uses the same words — one spelling of "folder"/"open" across the project.
QString   type_to_string(node_type t);
node_type type_from_string(const QString &s);

}  // namespace tree_outline
