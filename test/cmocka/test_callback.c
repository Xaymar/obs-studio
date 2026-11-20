#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <stdio.h>
#include <string.h>

#include <callback/calldata.h>
#include <callback/decl.h>
#include <callback/proc.h>
#include <callback/signal.h>

static void calldata_test(void **state)
{
	UNUSED_PARAMETER(state);

	calldata_t cd;
	calldata_init(&cd);
	assert_false(calldata_get_int(&cd, "missing", &(long long){0}));
	calldata_set_int(&cd, "i", -3);
	calldata_set_float(&cd, "f", 1.5);
	calldata_set_bool(&cd, "b", true);
	calldata_set_ptr(&cd, "p", &cd);
	calldata_set_string(&cd, "s", "hello");
	calldata_set_string(&cd, "null", NULL);

	assert_int_equal(calldata_int(&cd, "i"), -3);
	assert_true(calldata_float(&cd, "f") == 1.5);
	assert_true(calldata_bool(&cd, "b"));
	assert_ptr_equal(calldata_ptr(&cd, "p"), &cd);
	assert_string_equal(calldata_string(&cd, "s"), "hello");
	const char *str = "x";
	assert_true(calldata_get_string(&cd, "null", &str));
	assert_null(str);

	/* the getters check the size, so a different type is not found */
	bool b;
	assert_false(calldata_get_bool(&cd, "i", &b));
	assert_int_equal(calldata_int(&cd, "missing"), 0);
	assert_null(calldata_string(&cd, "missing"));

	/* growing and shrinking a value keeps the others intact */
	calldata_set_string(&cd, "s", "a much longer string than before, to grow the stack past its first capacity");
	assert_string_equal(calldata_string(&cd, "s"),
			    "a much longer string than before, to grow the stack past its first capacity");
	assert_int_equal(calldata_int(&cd, "i"), -3);
	assert_true(calldata_bool(&cd, "b"));
	calldata_set_string(&cd, "s", "");
	assert_string_equal(calldata_string(&cd, "s"), "");
	calldata_set_int(&cd, "i", 9);
	assert_int_equal(calldata_int(&cd, "i"), 9);
	assert_true(calldata_float(&cd, "f") == 1.5);
	assert_true(calldata_bool(&cd, "b"));

	/* many values */
	char name[16];
	for (int i = 0; i < 64; i++) {
		snprintf(name, sizeof(name), "v%d", i);
		calldata_set_int(&cd, name, i);
	}
	for (int i = 0; i < 64; i++) {
		snprintf(name, sizeof(name), "v%d", i);
		assert_int_equal(calldata_int(&cd, name), i);
	}

	calldata_clear(&cd);
	assert_false(calldata_get_int(&cd, "i", &(long long){0}));
	calldata_set_int(&cd, "i", 1);
	assert_int_equal(calldata_int(&cd, "i"), 1);

	/* bad names are ignored */
	calldata_set_int(&cd, NULL, 1);
	calldata_set_int(&cd, "", 1);
	assert_false(calldata_get_int(&cd, "", &(long long){0}));
	assert_false(calldata_get_int(&cd, NULL, &(long long){0}));
	assert_false(calldata_get_string(NULL, "s", &str));
	calldata_free(&cd);

	calldata_t *heap = calldata_create();
	calldata_set_int(heap, "i", 2);
	assert_int_equal(calldata_int(heap, "i"), 2);
	calldata_destroy(heap);
}

static void calldata_fixed_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t stack[64];
	calldata_t cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_int(&cd, "i", 5);
	assert_int_equal(calldata_int(&cd, "i"), 5);
	/* values that do not fit are dropped */
	calldata_set_string(&cd, "s", "this string is far too long for the 64 byte stack above");
	assert_null(calldata_string(&cd, "s"));
	calldata_set_string(&cd, "i", "this string is far too long for the 64 byte stack above");
	assert_int_equal(calldata_int(&cd, "i"), 5);
	calldata_free(&cd);
}

static void decl_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct decl_info decl = {0};
	assert_true(parse_decl_string(&decl, "void func(in int a, out float b, in out string c, ptr d, bool e)"));
	assert_string_equal(decl.name, "func");
	assert_int_equal(decl.params.num, 5);
	assert_string_equal(decl.params.array[0].name, "a");
	assert_int_equal(decl.params.array[0].type, CALL_PARAM_TYPE_INT);
	assert_int_equal(decl.params.array[0].flags, CALL_PARAM_IN);
	assert_int_equal(decl.params.array[1].flags, CALL_PARAM_OUT);
	assert_int_equal(decl.params.array[2].flags, CALL_PARAM_IN | CALL_PARAM_OUT);
	assert_int_equal(decl.params.array[2].type, CALL_PARAM_TYPE_STRING);
	assert_int_equal(decl.params.array[3].type, CALL_PARAM_TYPE_PTR);
	/* no specifier means input */
	assert_int_equal(decl.params.array[4].flags, CALL_PARAM_IN);
	assert_int_equal(decl.params.array[4].type, CALL_PARAM_TYPE_BOOL);
	decl_info_free(&decl);

	/* a return value becomes an out parameter named "return" */
	assert_true(parse_decl_string(&decl, "int get()"));
	assert_int_equal(decl.params.num, 1);
	assert_string_equal(decl.params.array[0].name, "return");
	assert_int_equal(decl.params.array[0].flags, CALL_PARAM_OUT);
	decl_info_free(&decl);

	static const char *bad[] = {
		"",
		"void",
		"void f",
		"nope f()",
		"void int()",
		"void f(int)",
		"void f(int a, int a)",
		"void f(int return)",
		"void f(in in int a)",
		"void f(out out int a)",
		"void f(void a)",
		"void f(int a",
		"void f(int a,",
		"void f(int a b)",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		print_message("decl: %s\n", bad[i]);
		assert_false(parse_decl_string(&decl, bad[i]));
		assert_null(decl.name);
		assert_int_equal(decl.params.num, 0);
	}
}

static int proc_calls;
static void *proc_data;
static void test_proc(void *data, calldata_t *cd)
{
	proc_calls++;
	proc_data = data;
	calldata_set_int(cd, "return", calldata_int(cd, "a") * 2);
}

static void proc_test(void **state)
{
	UNUSED_PARAMETER(state);

	int data;
	proc_calls = 0;
	proc_handler_t *ph = proc_handler_create();
	proc_handler_add(ph, "int twice(int a)", test_proc, &data);
	/* duplicates and bad declarations are ignored */
	proc_handler_add(ph, "int twice(int b)", test_proc, NULL);
	proc_handler_add(ph, "bad", test_proc, NULL);

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_int(&cd, "a", 21);
	assert_true(proc_handler_call(ph, "twice", &cd));
	assert_int_equal(calldata_int(&cd, "return"), 42);
	assert_ptr_equal(proc_data, &data);
	assert_false(proc_handler_call(ph, "missing", &cd));
	assert_int_equal(proc_calls, 1);

	/* Regression: a NULL name reached strcmp() */
	assert_false(proc_handler_call(ph, NULL, &cd));
	calldata_free(&cd);

	proc_handler_destroy(ph);
	proc_handler_destroy(NULL);
	proc_handler_add(NULL, "void f()", test_proc, NULL);
	assert_false(proc_handler_call(NULL, "twice", NULL));
}

struct sig_state {
	signal_handler_t *sh;
	int calls;
	int global_calls;
	int recursion;
	bool remove_self;
	bool disconnect_self;
	bool disconnect_global_self;
	const char *last_signal;
};

static void on_signal(void *data, calldata_t *cd)
{
	struct sig_state *s = data;
	s->calls++;
	calldata_set_int(cd, "seen", calldata_int(cd, "seen") + 1);
	if (s->remove_self)
		signal_handler_remove_current();
	if (s->disconnect_self)
		signal_handler_disconnect(s->sh, "sig", on_signal, data);
}

static void on_other(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct sig_state *s = data;
	s->calls += 100;
}

static void on_global(void *data, const char *signal, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct sig_state *s = data;
	s->global_calls++;
	s->last_signal = signal;
	if (s->remove_self)
		signal_handler_remove_current();
	if (s->disconnect_global_self)
		signal_handler_disconnect_global(s->sh, on_global, data);
}

static void signal_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct sig_state s = {0};
	signal_handler_t *sh = signal_handler_create();
	s.sh = sh;
	assert_true(signal_handler_add(sh, "void sig(int seen)"));
	assert_true(signal_handler_add(sh, "void other()"));
	assert_false(signal_handler_add(sh, "void sig()"));
	assert_false(signal_handler_add(sh, "bad"));

	calldata_t cd;
	calldata_init(&cd);

	/* connecting twice connects once */
	signal_handler_connect(sh, "sig", on_signal, &s);
	signal_handler_connect(sh, "sig", on_signal, &s);
	signal_handler_connect(sh, "missing", on_signal, &s);
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.calls, 1);
	assert_int_equal(calldata_int(&cd, "seen"), 1);
	signal_handler_signal(sh, "missing", &cd);

	signal_handler_connect(sh, "other", on_other, &s);
	signal_handler_signal(sh, "other", &cd);
	assert_int_equal(s.calls, 101);

	signal_handler_connect_global(sh, on_global, &s);
	signal_handler_connect_global(sh, on_global, &s);
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.global_calls, 1);
	assert_string_equal(s.last_signal, "sig");
	signal_handler_disconnect_global(sh, on_global, &s);
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.global_calls, 1);

	/* removing the current callback from inside it */
	s.calls = 0;
	s.remove_self = true;
	signal_handler_signal(sh, "sig", &cd);
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.calls, 1);
	s.remove_self = false;

	/* disconnecting from inside the callback */
	signal_handler_connect(sh, "sig", on_signal, &s);
	s.disconnect_self = true;
	s.calls = 0;
	signal_handler_signal(sh, "sig", &cd);
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.calls, 1);
	s.disconnect_self = false;
	signal_handler_disconnect(sh, "sig", on_signal, &s);

	/* removing a global callback from inside it */
	signal_handler_connect_global(sh, on_global, &s);
	s.remove_self = true;
	s.global_calls = 0;
	signal_handler_signal(sh, "other", &cd);
	signal_handler_signal(sh, "other", &cd);
	assert_int_equal(s.global_calls, 1);
	s.remove_self = false;

	/* Regression: disconnect_global from inside its own callback did nothing */
	signal_handler_connect_global(sh, on_global, &s);
	s.disconnect_global_self = true;
	s.global_calls = 0;
	signal_handler_signal(sh, "other", &cd);
	signal_handler_signal(sh, "other", &cd);
	assert_int_equal(s.global_calls, 1);
	s.disconnect_global_self = false;

	/* Regression: a NULL signal name reached strcmp() */
	signal_handler_connect(sh, NULL, on_signal, &s);
	signal_handler_disconnect(sh, NULL, on_signal, &s);
	signal_handler_signal(sh, NULL, &cd);

	calldata_free(&cd);
	signal_handler_destroy(sh);

	/* NULL handlers are ignored */
	signal_handler_destroy(NULL);
	signal_handler_connect(NULL, "sig", on_signal, &s);
	signal_handler_disconnect(NULL, "sig", on_signal, &s);
	signal_handler_signal(NULL, "sig", NULL);
	signal_handler_connect_global(NULL, on_global, &s);
	signal_handler_disconnect_global(NULL, on_global, &s);
	/* Regression: adding to a NULL handler dereferenced it */
	assert_false(signal_handler_add(NULL, "void sig()"));
	/* outside a callback this does nothing */
	signal_handler_remove_current();
}

static void signal_ref_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct sig_state s = {0};
	calldata_t cd;
	calldata_init(&cd);

	/* a ref connection keeps the handler alive until it is disconnected */
	signal_handler_t *sh = signal_handler_create();
	signal_handler_add(sh, "void sig()");
	signal_handler_connect_ref(sh, "sig", on_signal, &s);
	signal_handler_destroy(sh);
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.calls, 1);
	signal_handler_disconnect(sh, "sig", on_signal, &s);

	/* Regression: removing the last ref connection from inside its callback only
	 * lowered the count, and the handler leaked */
	sh = signal_handler_create();
	signal_handler_add(sh, "void sig()");
	signal_handler_connect_ref(sh, "sig", on_signal, &s);
	signal_handler_destroy(sh);
	s.remove_self = true;
	signal_handler_signal(sh, "sig", &cd);
	assert_int_equal(s.calls, 2);

	calldata_free(&cd);
}

struct nest_state {
	signal_handler_t *sh;
	int depth;
	int first_calls;
	int second_calls;
};

static void nest_first(void *data, calldata_t *cd)
{
	struct nest_state *n = data;
	n->first_calls++;
	if (n->depth++ == 0) {
		/* the nested call removes this callback; the outer loop must still reach the second one */
		signal_handler_remove_current();
		signal_handler_signal(n->sh, "sig", cd);
	}
}

static void nest_second(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct nest_state *n = data;
	n->second_calls++;
}

static void signal_nested_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* Regression: a nested signal of the same signal erased removed callbacks while the outer
	 * signal was still iterating, so the outer one skipped the next callback */
	struct nest_state n = {0};
	n.sh = signal_handler_create();
	signal_handler_add(n.sh, "void sig()");
	signal_handler_connect(n.sh, "sig", nest_first, &n);
	signal_handler_connect(n.sh, "sig", nest_second, &n);
	calldata_t cd;
	calldata_init(&cd);
	signal_handler_signal(n.sh, "sig", &cd);
	assert_int_equal(n.first_calls, 1);
	assert_int_equal(n.second_calls, 2);
	signal_handler_signal(n.sh, "sig", &cd);
	assert_int_equal(n.first_calls, 1);
	assert_int_equal(n.second_calls, 3);
	calldata_free(&cd);
	signal_handler_destroy(n.sh);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(calldata_test),      obs_unit_test(calldata_fixed_test), obs_unit_test(decl_test),
		obs_unit_test(proc_test),          obs_unit_test(signal_test),         obs_unit_test(signal_ref_test),
		obs_unit_test(signal_nested_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
