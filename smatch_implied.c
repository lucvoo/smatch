/*
 * sparse/smatch_implied.c
 *
 * Copyright (C) 2008 Dan Carpenter.
 *
 * Licensed under the Open Software License version 1.1
 *
 */

/*
 * Imagine we have this code:
 * foo = 1;
 * if (bar)
 *         foo = 99;
 * else
 *         frob();
 *                   //  <-- point #1
 * if (foo == 99)    //  <-- point #2
 *         bar->baz; //  <-- point #3
 *
 *
 * At point #3 bar is non null and can be dereferenced.
 *
 * It's smatch_implied.c which sets bar to non null at point #2.
 *
 * At point #1 merge_slist() stores the list of states from both
 * the true and false paths.  On the true path foo == 99 and on
 * the false path foo == 1.  merge_slist() sets their my_pool
 * list to show the other states which were there when foo == 99.
 *
 * When it comes to the if (foo == 99) the smatch implied hook
 * looks for all the pools where foo was not 99.  It makes a list
 * of those.
 * 
 * Then for bar (and all the other states) it says, ok bar is a
 * merged state that came from these previous states.  We'll 
 * chop out all the states where it came from a pool where 
 * foo != 99 and merge it all back together.
 *
 * That is the implied state of bar.
 *
 * merge_slist() sets up ->my_pool.  An sm_state only has one ->my_pool and
 *    that is the pool where it was first set.  The my pool gets set when
 *    code paths merge.  States that have been set since the last merge do 
 *    not have a ->my_pool.
 * merge_sm_state() sets ->left and ->right.  (These are the states which were
 *    merged to form the current state.)
 * a pool:  a pool is an slist that has been merged with another slist.
 */

#include <sys/time.h>
#include <time.h>
#include "smatch.h"
#include "smatch_slist.h"
#include "smatch_extra.h"

static int print_count = 0;
#define print_once(msg...) do { if (!print_count++) sm_msg(msg); } while (0)
#define DIMPLIED(msg...) do { if (option_debug_implied) printf(msg); } while (0)

int option_debug_implied = 0;
int option_no_implied = 0;

#define RIGHT 0
#define LEFT  1

/*
 * tmp_range_list(): 
 * It messes things up to free range list allocations.  This helper fuction
 * lets us reuse memory instead of doing new allocations.
 */
static struct range_list *tmp_range_list(long long num)
{
	static struct range_list *my_list = NULL;
	static struct data_range *my_range;

	__free_ptr_list((struct ptr_list **)&my_list);
	my_range = alloc_range(num, num);
	my_range->min = num;
	my_range->max = num;
	add_ptr_list(&my_list, my_range);
	return my_list;
}

/*
 * If 'foo' == 99 add it that pool to the true pools.  If it's false, add it to
 * the false pools.  If we're not sure, then we don't add it to either.
 */
static void do_compare(struct sm_state *sm_state, int comparison, struct range_list *vals,
			int lr,
			struct state_list_stack **true_stack,
			struct state_list_stack **false_stack)
{
	struct sm_state *s;
	int istrue;
	int isfalse;

 	if (!sm_state->my_pool)
		return;

	if (is_implied(sm_state)) {
		s = get_sm_state_slist(sm_state->my_pool,
				sm_state->owner, sm_state->name, 
				sm_state->sym);
	} else { 
		s = sm_state;
	}

	if (!s) {
		if (option_debug_implied || option_debug)
			sm_msg("%s from %d, has borrowed implications.",
				sm_state->name, sm_state->line);
		return;
	}

	istrue = !possibly_false_range_list_lr(comparison,	get_dinfo(s->state), vals, lr);
	isfalse = !possibly_true_range_list_lr(comparison, get_dinfo(s->state), vals, lr);
		
	if (option_debug_implied || option_debug) {
		if (istrue && isfalse) {
			printf("'%s = %s' from %d does not exist.\n", s->name, 
				show_state(s->state), s->line);
		} else if (istrue) {
			printf("'%s = %s' from %d is true.\n", s->name, show_state(s->state),
				s->line);
		} else if (isfalse) {
			printf("'%s = %s' from %d is false.\n", s->name, show_state(s->state),
				s->line);
		} else {
			printf("'%s = %s' from %d could be true or false.\n", s->name,
				show_state(s->state), s->line);
		}
	}
	if (istrue)
		add_pool(true_stack, s->my_pool);

	if (isfalse)
		add_pool(false_stack, s->my_pool);
}

static int pool_in_pools(struct state_list *pool,
			struct state_list_stack *pools)
{
	struct state_list *tmp;

	FOR_EACH_PTR(pools, tmp) {
		if (tmp == pool)
			return 1;
		if (tmp > pool)
			return 0;
	} END_FOR_EACH_PTR(tmp);
	return 0;
}

static int is_checked(struct state_list *checked, struct sm_state *sm)
{
	struct sm_state *tmp;

	FOR_EACH_PTR(checked, tmp) {
		if (tmp == sm)
			return 1;
	} END_FOR_EACH_PTR(tmp);
	return 0;
}

/*
 * separate_pools():
 * Example code:  if (foo == 99) {
 *
 * Say 'foo' is a merged state that has many possible values.  It is the combination
 * of merges.  separate_pools() iterates through the pools recursively and calls
 * do_compare() for each time 'foo' was set.
 */
static void separate_pools(struct sm_state *sm_state, int comparison, struct range_list *vals,
			int lr,
			struct state_list_stack **true_stack,
			struct state_list_stack **false_stack,
			struct state_list **checked)
{
	int free_checked = 0;
	struct state_list *checked_states = NULL;
 
	if (!sm_state)
		return;

	/* 
	   Sometimes the implications are just too big to deal with
	   so we bail.  Theoretically, bailing out here can cause more false 
	   positives but won't hide actual bugs.
	*/
	if (sm_state->nr_children > 4000) {
		print_once("debug: seperate_pools %s nr_children %d", sm_state->name, 
			sm_state->nr_children);
		return;
	}

	if (checked == NULL) {
		checked = &checked_states;
		free_checked = 1;
	}
	if (is_checked(*checked, sm_state))
		return;
	add_ptr_list(checked, sm_state);
	
	do_compare(sm_state, comparison, vals, lr, true_stack, false_stack);

	separate_pools(sm_state->left, comparison, vals, lr, true_stack, false_stack, checked);
	separate_pools(sm_state->right, comparison, vals, lr, true_stack, false_stack, checked);
	if (free_checked)
		free_slist(checked);
}

struct sm_state *remove_my_pools(struct sm_state *sm,
				struct state_list_stack *pools, int *modified)
{
	struct sm_state *ret = NULL;
	struct sm_state *left;
	struct sm_state *right;
	int removed = 0;

	if (!sm)
		return NULL;

	if (sm->nr_children > 4000) {
		print_once("debug: remove_my_pools %s nr_children %d", sm->name,
			sm->nr_children);
		return NULL;
	}

	if (pool_in_pools(sm->my_pool, pools)) {
		DIMPLIED("removed %s = %s from %d\n", sm->name,
			show_state(sm->state), sm->line);
		*modified = 1;
		return NULL;
	}

	if (!is_merged(sm)) {
		DIMPLIED("kept %s = %s from %d\n", sm->name, show_state(sm->state),
			sm->line);
		return sm;
	}

	DIMPLIED("checking %s = %s from %d\n", sm->name, show_state(sm->state), sm->line);
	left = remove_my_pools(sm->left, pools, &removed);
	right = remove_my_pools(sm->right, pools, &removed);
	if (!removed) {
		DIMPLIED("kept %s = %s from %d\n", sm->name, show_state(sm->state), sm->line);
		return sm;
	}
	*modified = 1;
	if (!left && !right) {
		DIMPLIED("removed %s = %s from %d\n", sm->name, show_state(sm->state), sm->line);
		return NULL;
	}

	if (!left) {
		ret = clone_sm(right);
		ret->merged = 1;
		ret->right = right;
		ret->left = NULL;
		ret->my_pool = sm->my_pool;
	} else if (!right) {
		ret = clone_sm(left);
		ret->merged = 1;
		ret->left = left;
		ret->right = NULL;
		ret->my_pool = sm->my_pool;
	} else {
		ret = merge_sm_states(left, right);
		ret->my_pool = sm->my_pool;
	}
	ret->implied = 1;
	DIMPLIED("partial %s = %s from %d\n", sm->name, show_state(sm->state), sm->line);
	return ret;
}

static struct state_list *filter_stack(struct state_list *pre_list,
				struct state_list_stack *stack)
{
	struct state_list *ret = NULL;
	struct sm_state *tmp;
	struct sm_state *filtered_sm;
	int modified;
	int counter = 0;

	if (!stack)
		return NULL;

	FOR_EACH_PTR(pre_list, tmp) {
		modified = 0;
		filtered_sm = remove_my_pools(tmp, stack, &modified);
		if (filtered_sm && modified) {
			filtered_sm->name = tmp->name;
			filtered_sm->sym = tmp->sym;
			add_ptr_list(&ret, filtered_sm);
			if ((counter++)%10 && out_of_memory())
				return NULL;

		}
	} END_FOR_EACH_PTR(tmp);
	return ret;
}

static void separate_and_filter(struct sm_state *sm_state, int comparison, struct range_list *vals,
		int lr,
		struct state_list *pre_list,
		struct state_list **true_states,
		struct state_list **false_states)
{
	struct state_list_stack *true_stack = NULL;
	struct state_list_stack *false_stack = NULL;
	struct timeval time_before;
	struct timeval time_after;

	gettimeofday(&time_before, NULL);

	if (!is_merged(sm_state)) {
		DIMPLIED("%d '%s' is not merged.\n", get_lineno(), sm_state->name);
		return;
	}

	if (option_debug_implied || option_debug) {
		if (lr == LEFT)
			sm_msg("checking implications: (%s %s %s)",
				sm_state->name, show_special(comparison), show_ranges(vals));
		else
			sm_msg("checking implications: (%s %s %s)",
				show_ranges(vals), show_special(comparison), sm_state->name);
	}

	separate_pools(sm_state, comparison, vals, lr, &true_stack, &false_stack, NULL);

	DIMPLIED("filtering true stack.\n");
	*true_states = filter_stack(pre_list, false_stack);
	DIMPLIED("filtering false stack.\n");
	*false_states = filter_stack(pre_list, true_stack);
	free_stack(&true_stack);
	free_stack(&false_stack);
	if (option_debug_implied || option_debug) {
		printf("These are the implied states for the true path:\n");
		__print_slist(*true_states);
		printf("These are the implied states for the false path:\n");
		__print_slist(*false_states);
	}

	gettimeofday(&time_after, NULL);
	if (time_after.tv_sec - time_before.tv_sec > 7)
		__bail_on_rest_of_function = 1;
}

static struct sm_state *get_left_most_sm(struct expression *expr)
{
	expr = strip_expr(expr);
	if (expr->type == EXPR_ASSIGNMENT)
		return get_left_most_sm(expr->left);
	return get_sm_state_expr(SMATCH_EXTRA, expr);
}

static int is_merged_expr(struct expression  *expr)
{
	struct sm_state *sm;
	long long dummy;

	if (get_value(expr, &dummy))
		return 0;
	sm = get_sm_state_expr(SMATCH_EXTRA, expr);
	if (!sm)
		return 0;
	if (is_merged(sm))
		return 1;
	return 0;
}

static void delete_equiv_slist(struct state_list **slist, const char *name, struct symbol *sym)
{
	struct smatch_state *state;
	struct data_info *dinfo;
	struct relation *rel;

	state = get_state(SMATCH_EXTRA, name, sym);
	dinfo = get_dinfo(state);
	if (!dinfo->related) {
		delete_state_slist(slist, SMATCH_EXTRA, name, sym);
		return;
	}

	FOR_EACH_PTR(dinfo->related, rel) {
		delete_state_slist(slist, SMATCH_EXTRA, rel->name, rel->sym);
	} END_FOR_EACH_PTR(rel);
}

static void handle_comparison(struct expression *expr,
			      struct state_list **implied_true,
			      struct state_list **implied_false)
{
	struct sm_state *sm = NULL;
	struct range_list *ranges = NULL;
	int lr;

	if (is_merged_expr(expr->left)) {
		lr = LEFT;
		sm = get_left_most_sm(expr->left);
		ranges = get_range_list(expr->right);
	} else if (is_merged_expr(expr->right)) {
		lr = RIGHT;
		sm = get_left_most_sm(expr->right);
		ranges = get_range_list(expr->left);
	}

	if (!ranges || !sm) {
		free_range_list(&ranges);
		return;
	}

	separate_and_filter(sm, expr->op, ranges, lr, __get_cur_slist(), implied_true, implied_false);
	free_range_list(&ranges);
	delete_equiv_slist(implied_true, sm->name, sm->sym);
	delete_equiv_slist(implied_false, sm->name, sm->sym);
}

static void handle_zero_comparison(struct expression *expr,
				struct state_list **implied_true,
				struct state_list **implied_false)
{
	struct symbol *sym;
	char *name;
	struct sm_state *sm;

	if (expr->type == EXPR_POSTOP)
		expr = strip_expr(expr->unop);

	if (expr->type == EXPR_ASSIGNMENT) {
		/* most of the time ->my_pools will be empty here because we
 		   just set the state, but if have assigned a conditional
		   function there are implications. */ 
		expr = expr->left;
	}

	name = get_variable_from_expr(expr, &sym);
	if (!name || !sym)
		goto free;
	sm = get_sm_state(SMATCH_EXTRA, name, sym);
	if (!sm)
		goto free;

	separate_and_filter(sm, SPECIAL_NOTEQUAL, tmp_range_list(0), LEFT, __get_cur_slist(), implied_true, implied_false);
	delete_equiv_slist(implied_true, name, sym);
	delete_equiv_slist(implied_false, name, sym);
free:
	free_string(name);
}

static void get_tf_states(struct expression *expr,
			  struct state_list **implied_true,
			  struct state_list **implied_false)
{
	if (expr->type == EXPR_COMPARE)
		handle_comparison(expr, implied_true, implied_false);
	else
		handle_zero_comparison(expr, implied_true, implied_false);
}

static void implied_states_hook(struct expression *expr)
{
	struct sm_state *sm;
	struct state_list *implied_true = NULL;
	struct state_list *implied_false = NULL;

	if (option_no_implied)
		return;

	get_tf_states(expr, &implied_true, &implied_false);

	FOR_EACH_PTR(implied_true, sm) {
		__set_true_false_sm(sm, NULL);
	} END_FOR_EACH_PTR(sm);
	free_slist(&implied_true);

	FOR_EACH_PTR(implied_false, sm) {
		__set_true_false_sm(NULL, sm);
	} END_FOR_EACH_PTR(sm);
	free_slist(&implied_false);
}

struct range_list *__get_implied_values(struct expression *switch_expr)
{
	char *name;
	struct symbol *sym;
	struct smatch_state *state;
	struct range_list *ret = NULL;

	name = get_variable_from_expr(switch_expr, &sym);
	if (!name || !sym)
		goto free;
	state = get_state(SMATCH_EXTRA, name, sym);
	if (!state)
		goto free;
	ret = clone_range_list(get_dinfo(state)->value_ranges);
free:
	free_string(name);
	if (!ret)
		add_range(&ret, whole_range.min, whole_range.max);
	return ret;
}

struct state_list *__implied_case_slist(struct expression *switch_expr,
					struct expression *case_expr,
					struct range_list_stack **remaining_cases,
					struct state_list **raw_slist)
{
	char *name = NULL;
	struct symbol *sym;
	struct sm_state *sm;
	struct sm_state *true_sm;
	struct state_list *true_states = NULL;
	struct state_list *false_states = NULL;
	struct state_list *ret = clone_slist(*raw_slist);
	long long val;
	struct data_range *range;
	struct range_list *vals = NULL;

	name = get_variable_from_expr(switch_expr, &sym);
	if (!name || !sym)
		goto free;
	sm = get_sm_state_slist(*raw_slist, SMATCH_EXTRA, name, sym);
	if (!case_expr) {
		vals = top_range_list(*remaining_cases);
	} else {
		if (!get_value(case_expr, &val)) {
			goto free;
		} else {
			filter_top_range_list(remaining_cases, val);
			range = alloc_range(val, val);
			add_ptr_list(&vals, range);
		}
	}
	if (sm)
		separate_and_filter(sm, SPECIAL_EQUAL, vals, LEFT, *raw_slist, &true_states, &false_states);
	
	true_sm = get_sm_state_slist(true_states, SMATCH_EXTRA, name, sym);
	if (!true_sm)
		set_state_slist(&true_states, SMATCH_EXTRA, name, sym, alloc_extra_state_range_list(vals));
	overwrite_slist(true_states, &ret);
	free_slist(&true_states);
	free_slist(&false_states);
free:
	free_string(name);
	return ret;
}

static void match_end_func(struct symbol *sym)
{
	print_count = 0;
}

/*
 * get_implications() can be called by check_ scripts.
 */
void get_implications(char *name, struct symbol *sym, int comparison, long long num,
		      struct state_list **true_states,
		      struct state_list **false_states)
{
	struct sm_state *sm;

	sm = get_sm_state(SMATCH_EXTRA, name, sym);
	if (!sm)
		return;
	if (slist_has_state(sm->possible, &undefined))
		return;
	separate_and_filter(sm, comparison, tmp_range_list(num), LEFT, __get_cur_slist(), true_states, false_states);
}

void __extra_match_condition(struct expression *expr);
void register_implications(int id)
{
	add_hook(&implied_states_hook, CONDITION_HOOK);
	add_hook(&__extra_match_condition, CONDITION_HOOK);
	add_hook(&match_end_func, END_FUNC_HOOK);
}