/*
 * sparse/smatch_function_hooks.c
 *
 * Copyright (C) 2009 Dan Carpenter.
 *
 * Licensed under the Open Software License version 1.1
 *
 */

/*
 * There are three types of function hooks:
 * add_function_hook()        - For any time a function is called.
 * add_function_assign_hook() - foo = the_function().
 * add_macro_assign_hook()    - foo = the_macro().
 * return_implies_state()     - For when a return value of 1 implies locked
 *                              and 0 implies unlocked. etc. etc.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include "smatch.h"
#include "smatch_slist.h"
#include "smatch_extra.h"
#include "smatch_function_hashtable.h"

struct fcall_back {
	int type;
	struct data_range *range;
	func_hook *call_back;
	void *info;
};

ALLOCATOR(fcall_back, "call backs");
DECLARE_PTR_LIST(call_back_list, struct fcall_back);

DEFINE_FUNCTION_HASHTABLE_STATIC(callback, struct fcall_back, struct call_back_list);
static struct hashtable *func_hash;

#define REGULAR_CALL     0
#define ASSIGN_CALL      1
#define RANGED_CALL      2
#define MACRO_ASSIGN     3

struct return_implies_callback {
	int type;
	return_implies_hook *callback;
};
ALLOCATOR(return_implies_callback, "return_implies callbacks");
DECLARE_PTR_LIST(db_implies_list, struct return_implies_callback);
static struct db_implies_list *db_implies_list;

static struct fcall_back *alloc_fcall_back(int type, func_hook *call_back,
					   void *info)
{
	struct fcall_back *cb;

	cb = __alloc_fcall_back(0);
	cb->type = type;
	cb->call_back = call_back;
	cb->info = info;
	return cb;
}

void add_function_hook(const char *look_for, func_hook *call_back, void *info)
{
	struct fcall_back *cb;

	cb = alloc_fcall_back(REGULAR_CALL, call_back, info);
	add_callback(func_hash, look_for, cb);
}

void add_function_assign_hook(const char *look_for, func_hook *call_back,
			      void *info)
{
	struct fcall_back *cb;

	cb = alloc_fcall_back(ASSIGN_CALL, call_back, info);
	add_callback(func_hash, look_for, cb);
}

void add_macro_assign_hook(const char *look_for, func_hook *call_back,
			void *info)
{
	struct fcall_back *cb;

	cb = alloc_fcall_back(MACRO_ASSIGN, call_back, info);
	add_callback(func_hash, look_for, cb);
}

void return_implies_state(const char *look_for, long long start, long long end,
			 implication_hook *call_back, void *info)
{
	struct fcall_back *cb;

	cb = alloc_fcall_back(RANGED_CALL, (func_hook *)call_back, info);
	cb->range = alloc_range_perm(start, end);
	add_callback(func_hash, look_for, cb);
}

void add_db_return_implies_callback(int type, return_implies_hook *callback)
{
	struct return_implies_callback *cb = __alloc_return_implies_callback(0);

	cb->type = type;
	cb->callback = callback;
	add_ptr_list(&db_implies_list, cb);
}

static void call_call_backs(struct call_back_list *list, int type,
			    const char *fn, struct expression *expr)
{
	struct fcall_back *tmp;

	FOR_EACH_PTR(list, tmp) {
		if (tmp->type == type)
			(tmp->call_back)(fn, expr, tmp->info);
	} END_FOR_EACH_PTR(tmp);
}

static void call_ranged_call_backs(struct call_back_list *list,
				const char *fn, struct expression *call_expr,
				struct expression *assign_expr)
{
	struct fcall_back *tmp;

	FOR_EACH_PTR(list, tmp) {
		((implication_hook *)(tmp->call_back))(fn, call_expr, assign_expr, tmp->info);
	} END_FOR_EACH_PTR(tmp);
}

static void match_function_call(struct expression *expr)
{
	struct call_back_list *call_backs;

	if (expr->fn->type != EXPR_SYMBOL || !expr->fn->symbol)
		return;
	call_backs = search_callback(func_hash, (char *)expr->fn->symbol->ident->name);
	if (!call_backs)
		return;
	call_call_backs(call_backs, REGULAR_CALL, expr->fn->symbol->ident->name,
			expr);
}

static struct call_back_list *get_same_ranged_call_backs(struct call_back_list *list,
						struct data_range *drange)
{
	struct call_back_list *ret = NULL;
	struct fcall_back *tmp;

	FOR_EACH_PTR(list, tmp) {
		if (tmp->type != RANGED_CALL)
			continue;
		if (tmp->range->min == drange->min && tmp->range->max == drange->max)
			add_ptr_list(&ret, tmp);
	} END_FOR_EACH_PTR(tmp);
	return ret;
}

static void assign_ranged_funcs(const char *fn, struct expression *expr,
				 struct call_back_list *call_backs)
{
	struct fcall_back *tmp;
	struct sm_state *sm;
	char *var_name;
	struct symbol *sym;
	struct smatch_state *estate;
	struct state_list *tmp_slist;
	struct state_list *final_states = NULL;
	struct range_list *handled_ranges = NULL;
	struct call_back_list *same_range_call_backs = NULL;

	var_name = get_variable_from_expr(expr->left, &sym);
	if (!var_name || !sym)
		goto free;

	FOR_EACH_PTR(call_backs, tmp) {
		if (tmp->type != RANGED_CALL)
			continue;
		if (in_list_exact(handled_ranges, tmp->range))
			continue;
		__push_fake_cur_slist();
		tack_on(&handled_ranges, tmp->range);

		same_range_call_backs = get_same_ranged_call_backs(call_backs, tmp->range);
		call_ranged_call_backs(same_range_call_backs, fn, expr->right, expr);
		__free_ptr_list((struct ptr_list **)&same_range_call_backs);

		estate = alloc_estate_range(tmp->range->min, tmp->range->max);
		set_extra_mod(var_name, sym, estate);

		tmp_slist = __pop_fake_cur_slist();
		merge_slist(&final_states, tmp_slist);
		free_slist(&tmp_slist);
	} END_FOR_EACH_PTR(tmp);

	FOR_EACH_PTR(final_states, sm) {
		__set_sm(sm);
	} END_FOR_EACH_PTR(sm);

	free_slist(&final_states);
free:
	free_string(var_name);
}

int call_implies_callbacks(int comparison, struct expression *expr, long long value, int left)
{
	struct call_back_list *call_backs;
	struct fcall_back *tmp;
	const char *fn;
	struct data_range *value_range;
	struct state_list *true_states = NULL;
	struct state_list *false_states = NULL;
	struct state_list *tmp_slist;
	struct sm_state *sm;

	if (expr->fn->type != EXPR_SYMBOL || !expr->fn->symbol)
		return 0;
	fn = expr->fn->symbol->ident->name;
	call_backs = search_callback(func_hash, (char *)expr->fn->symbol->ident->name);
	if (!call_backs)
		return 0;
	value_range = alloc_range(value, value);

	/* set true states */
	__push_fake_cur_slist();
	FOR_EACH_PTR(call_backs, tmp) {
		if (tmp->type != RANGED_CALL)
			continue;
		if (!true_comparison_range_lr(comparison, tmp->range, value_range, left))
			continue;
		((implication_hook *)(tmp->call_back))(fn, expr, NULL, tmp->info);
	} END_FOR_EACH_PTR(tmp);
	tmp_slist = __pop_fake_cur_slist();
	merge_slist(&true_states, tmp_slist);
	free_slist(&tmp_slist);

	/* set false states */
	__push_fake_cur_slist();
	FOR_EACH_PTR(call_backs, tmp) {
		if (tmp->type != RANGED_CALL)
			continue;
		if (!false_comparison_range_lr(comparison, tmp->range, value_range, left))
			continue;
		((implication_hook *)(tmp->call_back))(fn, expr, NULL, tmp->info);
	} END_FOR_EACH_PTR(tmp);
	tmp_slist = __pop_fake_cur_slist();
	merge_slist(&false_states, tmp_slist);
	free_slist(&tmp_slist);

	FOR_EACH_PTR(true_states, sm) {
		__set_true_false_sm(sm, NULL);
	} END_FOR_EACH_PTR(sm);
	FOR_EACH_PTR(false_states, sm) {
		__set_true_false_sm(NULL, sm);
	} END_FOR_EACH_PTR(sm);

	free_slist(&true_states);
	free_slist(&false_states);
	return 1;
}

struct db_callback_info {
	int true_side;
	int comparison;
	struct expression *expr;
	struct range_list *rl;
	int left;
	struct state_list *slist;
};
static struct db_callback_info db_info;
static int db_compare_callback(void *unused, int argc, char **argv, char **azColName)
{
	struct range_list *ret_range;
	int type, param;
	char *key, *value;
	struct return_implies_callback *tmp;

	if (argc != 5)
		return 0;

	get_value_ranges(argv[0], &ret_range);
	type = atoi(argv[1]);
	param = atoi(argv[2]);
	key = argv[3];
	value = argv[4];

	if (db_info.true_side) {
		if (!possibly_true_range_lists_rl(db_info.comparison,
						  ret_range, db_info.rl,
						  db_info.left))
			return 0;
	} else {
		if (!possibly_false_range_lists_rl(db_info.comparison,
						  ret_range, db_info.rl,
						  db_info.left))
			return 0;
	}

	FOR_EACH_PTR(db_implies_list, tmp) {
		if (tmp->type == type)
			tmp->callback(db_info.expr, param, key, value);
	} END_FOR_EACH_PTR(tmp);
	return 0;
}

void compare_db_implies_callbacks(int comparison, struct expression *expr, long long value, int left)
{
	struct symbol *sym;
        static char sql_filter[1024];
	struct state_list *true_states;
	struct state_list *false_states;
	struct sm_state *sm;

	if (expr->fn->type != EXPR_SYMBOL || !expr->fn->symbol)
		return;

	sym = expr->fn->symbol;
	if (!sym)
		return;

	if (sym->ctype.modifiers & MOD_STATIC) {
		snprintf(sql_filter, 1024,
			 "file = '%s' and function = '%s' and static = '1';",
			 get_filename(), sym->ident->name);
	} else {
		snprintf(sql_filter, 1024,
			 "function = '%s' and static = '0';", sym->ident->name);
	}

	db_info.comparison = comparison;
	db_info.expr = expr;
	db_info.rl = alloc_range_list(value, value);
	db_info.left = left;

	db_info.true_side = 1;
	__push_fake_cur_slist();
	run_sql(db_compare_callback,
		"select return, type, parameter, key, value from return_implies where %s",
		sql_filter);
	true_states = __pop_fake_cur_slist();

	db_info.true_side = 0;
	__push_fake_cur_slist();
	run_sql(db_compare_callback,
		"select return, type, parameter, key, value from return_implies where %s",
		sql_filter);
	false_states = __pop_fake_cur_slist();

	FOR_EACH_PTR(true_states, sm) {
		__set_true_false_sm(sm, NULL);
	} END_FOR_EACH_PTR(sm);
	FOR_EACH_PTR(false_states, sm) {
		__set_true_false_sm(NULL, sm);
	} END_FOR_EACH_PTR(sm);

	free_slist(&true_states);
	free_slist(&false_states);
}

void function_comparison(int comparison, struct expression *expr,
				long long value, int left)
{
	if (call_implies_callbacks(comparison, expr, value, left))
		return;
	compare_db_implies_callbacks(comparison, expr, value, left);
}

static int db_assign_callback(void *unused, int argc, char **argv, char **azColName)
{
	struct range_list *ret_range;
	int type, param;
	char *key, *value;
	struct return_implies_callback *tmp;
	struct state_list *slist;

	if (argc != 5)
		return 0;

	get_value_ranges(argv[0], &ret_range);
	type = atoi(argv[1]);
	param = atoi(argv[2]);
	key = argv[3];
	value = argv[4];

	__push_fake_cur_slist();
	FOR_EACH_PTR(db_implies_list, tmp) {
		if (tmp->type == type)
			tmp->callback(db_info.expr->right, param, key, value);
	} END_FOR_EACH_PTR(tmp);
	set_extra_expr_mod(db_info.expr->left, alloc_estate_range_list(ret_range));
	slist = __pop_fake_cur_slist();

	merge_slist(&db_info.slist, slist);

	return 0;
}

static void db_return_implies_assign(struct expression *expr)
{
	struct symbol *sym;
        static char sql_filter[1024];
	static struct sm_state *sm;

	if (expr->right->fn->type != EXPR_SYMBOL || !expr->right->fn->symbol)
		return;

	sym = expr->right->fn->symbol;
	if (!sym)
		return;

	if (sym->ctype.modifiers & MOD_STATIC) {
		snprintf(sql_filter, 1024,
			 "file = '%s' and function = '%s' and static = '1';",
			 get_filename(), sym->ident->name);
	} else {
		snprintf(sql_filter, 1024,
			 "function = '%s' and static = '0';", sym->ident->name);
	}

	db_info.expr = expr;
	db_info.slist = NULL;
	run_sql(db_assign_callback,
		"select return, type, parameter, key, value from return_implies where %s",
		sql_filter);

	FOR_EACH_PTR(db_info.slist, sm) {
		__set_sm(sm);
	} END_FOR_EACH_PTR(sm);

}

static void match_assign_call(struct expression *expr)
{
	struct call_back_list *call_backs;
	const char *fn;
	struct expression *right;

	right = strip_expr(expr->right);
	if (right->fn->type != EXPR_SYMBOL || !right->fn->symbol)
		return;
	fn = right->fn->symbol->ident->name;
	call_backs = search_callback(func_hash, (char *)fn);
	if (!call_backs) {
		db_return_implies_assign(expr);
		return;
	}
	call_call_backs(call_backs, ASSIGN_CALL, fn, expr);
	assign_ranged_funcs(fn, expr, call_backs);
}

static void match_macro_assign(struct expression *expr)
{
	struct call_back_list *call_backs;
	const char *macro;
	struct expression *right;

	right = strip_expr(expr->right);
	macro = get_macro_name(right->pos);
	call_backs = search_callback(func_hash, (char *)macro);
	if (!call_backs)
		return;
	call_call_backs(call_backs, MACRO_ASSIGN, macro, expr);
}

void create_function_hook_hash(void)
{
	func_hash = create_function_hashtable(5000);
}

void register_function_hooks(int id)
{
	add_hook(&match_function_call, FUNCTION_CALL_HOOK);
	add_hook(&match_assign_call, CALL_ASSIGNMENT_HOOK);
	add_hook(&match_macro_assign, MACRO_ASSIGNMENT_HOOK);
}