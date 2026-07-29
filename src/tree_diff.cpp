// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree_diff.h"
#include "node.h"

#include <QSet>

namespace {

void collect(node *n, QList<node *> &out, bool leaves_only) {
	for (node *c : n->children) {
		if (!leaves_only || !c->is_folder())
			out.push_back(c);
		collect(c, out, leaves_only);
	}
}

QList<node *> all_nodes(node *root) {
	QList<node *> out;
	if (root)
		collect(root, out, false);
	return out;
}

QList<node *> all_leaves(node *root) {
	QList<node *> out;
	if (root)
		collect(root, out, true);
	return out;
}

// Detach a node from its parent's child list without deleting it.
void detach(node *n) {
	if (n && n->parent)
		n->parent->children.removeAll(n);
}

void renumber(node *parent) {
	for (int i = 0; i < parent->children.size(); ++i)
		parent->children[i]->order = i;
}

}  // namespace

namespace tree_diff {

QStringList leaf_ids(node *root) {
	QStringList ids;
	for (node *n : all_leaves(root))
		ids << n->id;
	return ids;
}

proposal_report check_and_repair(node *original, node *proposal) {
	proposal_report rep;
	if (!original || !proposal) {
		rep.message = "No proposal to check.";
		return rep;
	}

	QHash<QString, node *> original_by_id;
	for (node *n : all_nodes(original))
		original_by_id.insert(n->id, n);

	// --- Pass 1: walk the proposal, dropping duplicates and invented leaves.
	QSet<QString> seen;
	QList<node *> doomed;
	for (node *n : all_nodes(proposal)) {
		if (seen.contains(n->id)) {
			rep.duplicated_ids << n->id;
			doomed << n;                  // keep the first occurrence only
			continue;
		}
		seen.insert(n->id);

		node *orig = original_by_id.value(n->id, nullptr);
		if (!orig) {
			if (n->is_folder()) {
				rep.new_folders++;        // folders are the model's to invent
			} else {
				// A leaf id that never existed is a fabricated tab. There is no
				// safe repair, so the whole proposal fails.
				rep.invented_ids << n->id;
			}
			continue;
		}
		// An existing id that changed kind (leaf <-> folder) would silently
		// convert a tab into a folder or vice versa. Treat it as invented.
		if (orig->is_folder() != n->is_folder())
			rep.invented_ids << n->id;
	}

	if (!rep.invented_ids.isEmpty()) {
		rep.usable = false;
		rep.message = QString("Rejected: the proposal contains %1 tab id(s) that "
		                      "do not exist in the tree (%2).")
		                  .arg(rep.invented_ids.size())
		                  .arg(rep.invented_ids.join(", "));
		return rep;
	}

	for (node *d : doomed) {
		detach(d);
		delete d;   // takes its subtree; those ids re-appear as dropped below
	}

	// --- Pass 2: re-attach any original leaf the model omitted.
	// Recompute what survives after the duplicate cull, so a leaf that only
	// existed inside a deleted duplicate subtree is still recovered.
	QSet<QString> present;
	for (node *n : all_nodes(proposal))
		present.insert(n->id);

	for (node *orig : all_leaves(original)) {
		if (present.contains(orig->id))
			continue;
		rep.dropped_ids << orig->id;

		// Put it back where it was if that parent survived, else at the root.
		node *dest = proposal;
		if (orig->parent && present.contains(orig->parent->id)) {
			for (node *cand : all_nodes(proposal)) {
				if (cand->id == orig->parent->id) { dest = cand; break; }
			}
		}
		node *copy   = new node;
		copy->id     = orig->id;
		copy->type   = orig->type;
		copy->title  = orig->title;
		copy->url    = orig->url;
		copy->tags   = orig->tags;
		copy->parent = dest;
		copy->order  = dest->children.size();
		dest->children.push_back(copy);
		present.insert(copy->id);
	}

	for (node *n : all_nodes(proposal))
		renumber(n);
	renumber(proposal);

	rep.usable = true;
	QStringList notes;
	if (!rep.dropped_ids.isEmpty())
		notes << QString("%1 dropped tab(s) restored").arg(rep.dropped_ids.size());
	if (!rep.duplicated_ids.isEmpty())
		notes << QString("%1 duplicate(s) collapsed").arg(rep.duplicated_ids.size());
	if (rep.new_folders)
		notes << QString("%1 new folder(s)").arg(rep.new_folders);
	rep.message = notes.isEmpty() ? "Proposal is clean." : notes.join("; ") + ".";
	return rep;
}

QList<tree_change> compute(node *original, node *proposal) {
	QList<tree_change> changes;
	if (!original || !proposal)
		return changes;

	QHash<QString, node *> orig_by_id;
	for (node *n : all_nodes(original))
		orig_by_id.insert(n->id, n);

	for (node *n : all_nodes(proposal)) {
		const QString parent_id = (n->parent && n->parent != proposal)
		                              ? n->parent->id : QString("root");
		node *orig = orig_by_id.value(n->id, nullptr);

		if (!orig) {
			tree_change c;
			c.kind          = change_kind::folder_new;
			c.node_id       = n->id;
			c.new_parent_id = parent_id;
			c.new_order     = n->order;
			c.new_title     = n->title;
			c.summary = QString("New folder \"%1\" in %2").arg(n->title, parent_id);
			changes << c;
			continue;
		}

		const QString orig_parent_id = (orig->parent && orig->parent != original)
		                                   ? orig->parent->id : QString("root");
		if (orig_parent_id != parent_id) {
			tree_change c;
			c.kind          = change_kind::reparented;
			c.node_id       = n->id;
			c.new_parent_id = parent_id;
			c.new_order     = n->order;
			c.summary = QString("Move \"%1\": %2 → %3")
			                .arg(orig->title.isEmpty() ? orig->id : orig->title,
			                     orig_parent_id, parent_id);
			changes << c;
		} else if (orig->order != n->order) {
			tree_change c;
			c.kind          = change_kind::reordered;
			c.node_id       = n->id;
			c.new_parent_id = parent_id;
			c.new_order     = n->order;
			c.summary = QString("Reorder \"%1\" in %2: %3 → %4")
			                .arg(orig->title.isEmpty() ? orig->id : orig->title,
			                     parent_id)
			                .arg(orig->order).arg(n->order);
			changes << c;
		}

		if (orig->is_folder() && orig->title != n->title) {
			tree_change c;
			c.kind      = change_kind::folder_renamed;
			c.node_id   = n->id;
			c.new_title = n->title;
			c.summary   = QString("Rename folder \"%1\" → \"%2\"")
			                  .arg(orig->title, n->title);
			changes << c;
		}
	}

	// Duplicate URLs, offered for merge rather than acted on (§9.5).
	QHash<QString, QString> first_by_url;   // url -> id
	for (node *n : all_leaves(proposal)) {
		if (n->url.isEmpty())
			continue;
		if (first_by_url.contains(n->url)) {
			tree_change c;
			c.kind     = change_kind::duplicate_url;
			c.node_id  = n->id;
			c.accepted = false;   // never pre-selected; merging is destructive
			c.summary  = QString("Duplicate URL: \"%1\" also at %2")
			                 .arg(n->url, first_by_url.value(n->url));
			changes << c;
		} else {
			first_by_url.insert(n->url, n->id);
		}
	}

	return changes;
}

int apply(node *original, const QList<tree_change> &changes) {
	if (!original)
		return 0;

	QHash<QString, node *> by_id;
	by_id.insert("root", original);
	for (node *n : all_nodes(original))
		by_id.insert(n->id, n);

	int applied = 0;

	// New folders first, so a move into one has somewhere to land.
	for (const tree_change &c : changes) {
		if (!c.accepted || c.kind != change_kind::folder_new)
			continue;
		node *parent = by_id.value(c.new_parent_id, original);
		node *f   = new node;
		f->id     = c.node_id;
		f->type   = node_type::folder;
		f->title  = c.new_title;
		f->parent = parent;
		parent->children.push_back(f);
		by_id.insert(f->id, f);
		++applied;
	}

	for (const tree_change &c : changes) {
		if (!c.accepted)
			continue;
		node *n = by_id.value(c.node_id, nullptr);
		if (!n)
			continue;

		switch (c.kind) {
			case change_kind::reparented:
			case change_kind::reordered: {
				node *parent = by_id.value(c.new_parent_id, original);
				if (!parent || parent == n)
					break;
				// Refuse to move a node inside its own subtree.
				for (node *p = parent; p; p = p->parent)
					if (p == n) { parent = nullptr; break; }
				if (!parent)
					break;
				detach(n);
				n->parent = parent;
				const int at = qBound(0, c.new_order, parent->children.size());
				parent->children.insert(at, n);
				++applied;
				break;
			}
			case change_kind::folder_renamed:
				n->title = c.new_title;
				++applied;
				break;
			case change_kind::folder_new:      // already handled above
			case change_kind::duplicate_url:   // advisory only
				break;
		}
	}

	renumber(original);
	for (node *n : all_nodes(original))
		renumber(n);
	return applied;
}

}  // namespace tree_diff
