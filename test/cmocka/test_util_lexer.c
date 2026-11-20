#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/lexer.h>

static void strref_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct strref a, b, empty;
	strref_set(&a, "Hello World", 5);
	strref_clear(&empty);
	assert_true(strref_is_empty(&empty));
	assert_true(strref_is_empty(NULL));
	assert_false(strref_is_empty(&a));

	assert_int_equal(strref_cmp(&a, "Hello"), 0);
	assert_int_equal(strref_cmp(&a, "Help"), -1);
	assert_int_equal(strref_cmp(&a, "Hell"), 1);
	assert_int_equal(strref_cmp(&a, "Hello!"), -1);
	assert_int_equal(strref_cmp(&a, NULL), 1);
	assert_int_equal(strref_cmp(&empty, NULL), 0);
	assert_int_equal(strref_cmp(&empty, ""), 0);
	assert_int_equal(strref_cmp(&empty, "x"), -1);

	assert_int_equal(strref_cmpi(&a, "HELLO"), 0);
	assert_int_equal(strref_cmpi(&a, "help"), -1);
	assert_int_equal(strref_cmpi(&a, "HELL"), 1);
	assert_int_equal(strref_cmpi(&a, NULL), 1);
	assert_int_equal(strref_cmpi(&empty, NULL), 0);
	assert_int_equal(strref_cmpi(&empty, "x"), -1);

	strref_set(&b, "Hello there", 5);
	assert_int_equal(strref_cmp_strref(&a, &b), 0);
	assert_int_equal(strref_cmp_strref(&empty, &empty), 0);
	assert_int_equal(strref_cmp_strref(&empty, &a), -1);
	assert_int_equal(strref_cmp_strref(&a, &empty), -1);
	strref_set(&b, "Help", 4);
	assert_int_equal(strref_cmp_strref(&a, &b), -1);
	assert_int_equal(strref_cmp_strref(&b, &a), 1);
	strref_set(&b, "Hello!", 6);
	assert_int_equal(strref_cmp_strref(&a, &b), -1);

	strref_set(&b, "HELLO", 5);
	assert_int_equal(strref_cmpi_strref(&a, &b), 0);
	assert_int_equal(strref_cmpi_strref(&empty, &empty), 0);
	assert_int_equal(strref_cmpi_strref(&empty, &a), -1);
	assert_int_equal(strref_cmpi_strref(&a, &empty), -1);
	strref_set(&b, "HELP", 4);
	assert_int_equal(strref_cmpi_strref(&a, &b), -1);
	assert_int_equal(strref_cmpi_strref(&b, &a), 1);

	struct strref sum;
	strref_clear(&sum);
	strref_add(&sum, &a);
	strref_set(&b, " World", 6);
	strref_add(&sum, &b);
	assert_int_equal(sum.len, 11);
	struct strref copy;
	strref_copy(&copy, &sum);
	assert_int_equal(strref_cmp(&copy, "Hello World"), 0);
}

static void number_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(valid_int_str("123", 0));
	assert_true(valid_int_str("-123", 0));
	assert_true(valid_int_str("+7", 0));
	assert_true(valid_int_str("12x", 2));
	assert_false(valid_int_str("12x", 0));
	assert_false(valid_int_str("-", 0));
	assert_false(valid_int_str("", 0));
	assert_false(valid_int_str(NULL, 0));

	/* Regression: the sign counts toward n */
	assert_true(valid_int_str("-1 ", 2));
	assert_false(valid_int_str("-1", 1));
	assert_true(valid_float_str("-1.5 ", 4));

	assert_true(valid_float_str("1.5", 0));
	/* Regression: signed exponents were rejected */
	assert_true(valid_float_str("1e-3", 0));
	assert_true(valid_float_str("2.5e+3", 0));
	assert_false(valid_float_str("1e-", 0));
	assert_false(valid_float_str("1e3-2", 0));
	assert_false(valid_float_str("1e--3", 0));
	assert_true(valid_float_str("2e5", 0));
	assert_false(valid_float_str(".5", 0));
	assert_false(valid_float_str("1.2.3", 0));
	assert_false(valid_float_str("1e2.0", 0));
	assert_false(valid_float_str("1e2e3", 0));
	assert_false(valid_float_str("e5", 0));
	assert_false(valid_float_str("1-2", 0));
	assert_false(valid_float_str("1e", 0));
	assert_false(valid_float_str("1x", 0));
	assert_false(valid_float_str("", 0));
	assert_false(valid_float_str(NULL, 0));

	struct strref ref;
	strref_set(&ref, "42;", 2);
	assert_true(valid_int_strref(&ref));
	strref_set(&ref, "4.2;", 3);
	assert_true(valid_float_strref(&ref));
}

static void char_class_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(is_whitespace(' '));
	assert_false(is_whitespace('a'));
	assert_true(is_newline('\r'));
	assert_true(is_space_or_tab('\t'));
	assert_true(is_newline_pair('\r', '\n'));
	assert_true(is_newline_pair('\n', '\r'));
	assert_false(is_newline_pair('\n', '\n'));
	assert_int_equal(newline_size("\r\nx"), 2);
	assert_int_equal(newline_size("\n\rx"), 2);
	assert_int_equal(newline_size("\nx"), 1);
	assert_int_equal(newline_size("x"), 0);
}

static void error_data_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct error_data ed;
	error_data_init(&ed);
	assert_false(error_data_has_errors(&ed));

	error_data_add(&ed, "file.txt", 1, 2, "a warning", LEX_WARNING);
	assert_false(error_data_has_errors(&ed));
	error_data_add(&ed, "file.txt", 3, 4, "an error", LEX_ERROR);
	error_data_add(NULL, "file.txt", 3, 4, "dropped", LEX_ERROR);
	assert_true(error_data_has_errors(&ed));
	assert_int_equal(error_data_type_count(&ed, LEX_WARNING), 1);
	assert_int_equal(error_data_type_count(&ed, LEX_ERROR), 1);
	assert_string_equal(error_data_item(&ed, 1)->error, "an error");

	char *text = error_data_buildstring(&ed);
	assert_string_equal(text, "file.txt (1, 2): a warning\nfile.txt (3, 4): an error\n");
	bfree(text);

	error_data_free(&ed);
	assert_int_equal(ed.errors.num, 0);
}

static void base_token_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct lexer lex;
	struct base_token token;
	lexer_init(&lex);
	assert_false(lexer_getbasetoken(&lex, &token, IGNORE_WHITESPACE));

	lexer_start(&lex, "abc 12\r\n+ x");
	const enum base_token_type expected[] = {BASETOKEN_ALPHA,      BASETOKEN_WHITESPACE, BASETOKEN_DIGIT,
						 BASETOKEN_WHITESPACE, BASETOKEN_OTHER,      BASETOKEN_WHITESPACE,
						 BASETOKEN_ALPHA};
	const char *texts[] = {"abc", " ", "12", "\r\n", "+", " ", "x"};
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		assert_true(lexer_getbasetoken(&lex, &token, PARSE_WHITESPACE));
		assert_int_equal(token.type, expected[i]);
		assert_int_equal(strref_cmp(&token.text, texts[i]), 0);
	}
	assert_false(lexer_getbasetoken(&lex, &token, PARSE_WHITESPACE));

	lexer_reset(&lex);
	assert_true(lexer_getbasetoken(&lex, &token, IGNORE_WHITESPACE));
	assert_true(lexer_getbasetoken(&lex, &token, IGNORE_WHITESPACE));
	assert_int_equal(strref_cmp(&token.text, "12"), 0);

	/* trailing whitespace only */
	lexer_start_move(&lex, bstrdup("   "));
	assert_false(lexer_getbasetoken(&lex, &token, IGNORE_WHITESPACE));

	struct base_token copy;
	base_token_copy(&copy, &token);
	base_token_clear(&copy);
	assert_int_equal(copy.type, BASETOKEN_NONE);

	lexer_free(&lex);
}

static void stroffset_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct lexer lex;
	lexer_init(&lex);
	lexer_start(&lex, "ab\r\ncd\nef");

	uint32_t row = 0, col = 0;
	lexer_getstroffset(&lex, lex.text + 8, &row, &col);
	assert_int_equal(row, 3);
	assert_int_equal(col, 2);

	lexer_getstroffset(&lex, lex.text, &row, &col);
	assert_int_equal(row, 1);
	assert_int_equal(col, 1);

	row = 99;
	lexer_getstroffset(&lex, NULL, &row, &col);
	assert_int_equal(row, 99);

	lexer_free(&lex);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(strref_test),     cmocka_unit_test(number_test),     cmocka_unit_test(char_class_test),
		cmocka_unit_test(error_data_test), cmocka_unit_test(base_token_test), cmocka_unit_test(stroffset_test),
	};

	int ret = cmocka_run_group_tests(tests, NULL, NULL);
	return ret ? ret : (bnum_allocs() != 0);
}
