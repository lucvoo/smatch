/*
 * sparse/smatch_constraints.c
 *
 * Copyright (C) 2010 Dan Carpenter.
 *
 * Licensed under the Open Software License version 1.1
 *
 */

/*
 * smatch_constraints.c is for tracking how variables are related
 *
 * if (a == b) {
 * if (a > b) {
 * if (a != b) {
 *
 * This is stored in a field in the smatch_extra dinfo.
 *
 * Normally the way that variables become related is through a
 * condition and you say:  add_constraint_expr(left, '<', right);
 * The other way it can happen is if you have an assignment:
 * set_equiv(left, right);
 *
 * One two variables "a" and "b" are related if then if we find
 * that "a" is greater than 0 we need to update "b".
 *
 * When a variable gets modified all the old relationships are
 * deleted.  remove_contraints(expr);
 *
 * Also we need an is_true_constraint(left, '<', right) and
 * is_false_constraint (left, '<', right).  This is used by
 * smatch_implied.
 *
 */

#include "smatch.h"
#include "smatch_slist.h"
#include "smatch_extra.h"

ALLOCATOR(relation, "related variables");

/*
 * set_equiv() is only used for assignments where we set one variable
 * equal to the other.  a = b;.  It's not used for if conditions where
 * a == b.
 */
void set_equiv(struct expression *left, struct expression *right)
{
	struct sm_state *right_sm;
	struct smatch_state *state;
	struct relation *rel;
	char *left_name;
	struct symbol *left_sym;

	left_name = get_variable_from_expr(left, &left_sym);
	if (!left_name || !left_sym)
		goto free;

	right_sm = get_sm_state_expr(SMATCH_EXTRA, right);
	if (!right_sm)
		right_sm = set_state_expr(SMATCH_EXTRA, right, extra_undefined());
	if (!right_sm)
		return;

	remove_from_equiv(left_name, left_sym);

	state = clone_estate(right_sm->state);
	if (!estate_related(state))
		add_equiv(state, right_sm->name, right_sm->sym);
	add_equiv(state, left_name, left_sym);

	FOR_EACH_PTR(estate_related(state), rel) {
		struct sm_state *new_sm;

		new_sm = clone_sm(right_sm);
		new_sm->name = rel->name;
		new_sm->sym = rel->sym;
		new_sm->state = state;
		__set_sm(new_sm);
	} END_FOR_EACH_PTR(rel);
free:
	free_string(left_name);
}

static struct relation *alloc_relation(int op, const char *name, struct symbol *sym)
{
	struct relation *tmp;

	tmp = __alloc_relation(0);
	tmp->op = op;
	tmp->name = alloc_string(name);
	tmp->sym = sym;
	return tmp;
}

struct related_list *clone_related_list(struct related_list *related)
{
	struct relation *rel;
	struct related_list *to_list = NULL;

	FOR_EACH_PTR(related, rel) {
		add_ptr_list(&to_list, rel);
	} END_FOR_EACH_PTR(rel);

	return to_list;
}

struct relation *get_common_relationship(struct smatch_state *state, int op,
					const char *name, struct symbol *sym)
{
	struct relation *tmp;

	// FIXME...
	// Find the common x < y and x <= y
	FOR_EACH_PTR(estate_related(state), tmp) {
		if (tmp->op < op || tmp->sym < sym || strcmp(tmp->name, name) < 0)
			continue;
		if (tmp->op == op && tmp->sym == sym && !strcmp(tmp->name, name))
			return tmp;
		return NULL;
	} END_FOR_EACH_PTR(tmp);
	return NULL;
}

static void debug_addition(struct smatch_state *state, int op, const char *name)
{
	struct relation *tmp;

	if (!option_debug_related)
		return;

	sm_prefix();
	sm_printf("(");
	FOR_EACH_PTR(estate_related(state), tmp) {
		sm_printf("%s %s ", show_special(tmp->op), tmp->name);
	} END_FOR_EACH_PTR(tmp);
	sm_printf(") <-- %s %s\n", show_special(op), name);
}

void add_related(struct smatch_state *state, int op, const char *name, struct symbol *sym)
{
	struct data_info *dinfo;
	struct relation *tmp;
	struct relation *new;

	debug_addition(state, op, name);

	dinfo = get_dinfo(state);
	FOR_EACH_PTR(dinfo->related, tmp) {
		if (tmp->op < op || tmp->sym < sym || strcmp(tmp->name, name) < 0)
			continue;
		if (tmp->op == op && tmp->sym == sym && !strcmp(tmp->name, name))
			return;
		new = alloc_relation(op, name, sym);
		INSERT_CURRENT(new, tmp);
		return;
	} END_FOR_EACH_PTR(tmp);
	new = alloc_relation(op, name, sym);
	add_ptr_list(&dinfo->related, new);
}

void del_related(struct smatch_state *state, int op, const char *name, struct symbol *sym)
{
	struct relation *tmp;

	FOR_EACH_PTR(estate_related(state), tmp) {
		if (tmp->sym < sym || strcmp(tmp->name, name) < 0)
			continue;
		if (tmp->sym == sym && !strcmp(tmp->name, name)) {
			DELETE_CURRENT_PTR(tmp);
			continue;
		}
		return;
	} END_FOR_EACH_PTR(tmp);
}

void add_equiv(struct smatch_state *state, const char *name, struct symbol *sym)
{
	add_related(state, SPECIAL_EQUAL, name, sym);
}

static void del_equiv(struct smatch_state *state, const char *name, struct symbol *sym)
{
	del_related(state, SPECIAL_EQUAL, name, sym);
}

void remove_from_equiv(const char *name, struct symbol *sym)
{
	struct sm_state *orig_sm;
	struct relation *rel;
	struct smatch_state *state;
	struct related_list *to_update;

	// FIXME equiv => related
	orig_sm = get_sm_state(SMATCH_EXTRA, name, sym);
	if (!orig_sm || !get_dinfo(orig_sm->state)->related)
		return;

	state = clone_estate(orig_sm->state);
	del_equiv(state, name, sym);
	to_update = get_dinfo(state)->related;
	if (ptr_list_size((struct ptr_list *)get_dinfo(state)->related) == 1)
		get_dinfo(state)->related = NULL;

	FOR_EACH_PTR(to_update, rel) {
		struct sm_state *new_sm;

		new_sm = clone_sm(orig_sm);
		new_sm->name = rel->name;
		new_sm->sym = rel->sym;
		new_sm->state = state;
		__set_sm(new_sm);
	} END_FOR_EACH_PTR(rel);
}

void remove_from_equiv_expr(struct expression *expr)
{
	char *name;
	struct symbol *sym;

	name = get_variable_from_expr(expr, &sym);
	if (!name || !sym)
		goto free;
	remove_from_equiv(name, sym);
free:
	free_string(name);
}

void add_constrain_expr(struct expression *left, int op, struct expression *right)
{

}

void set_equiv_state_expr(int id, struct expression *expr, struct smatch_state *state)
{
	struct relation *rel;
	struct smatch_state *estate;

	estate = get_state_expr(SMATCH_EXTRA, expr);

	if (!estate)
		return;

	FOR_EACH_PTR(get_dinfo(estate)->related, rel) {
		if (rel->op != SPECIAL_EQUAL)
			continue;
		set_state(id, rel->name, rel->sym, state);
	} END_FOR_EACH_PTR(rel);
}