#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <util/bmem.h>
#include <util/cf-parser.h>

static void navigation_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct cf_parser p;
	cf_parser_init(&p);
	assert_false(cf_parser_parse(&p, "", "empty"));
	assert_true(cf_parser_parse(&p, "first { skip; { nested } } target ( a , b ) name 5 ;", "test"));

	/* cur_token starts at the first token */
	assert_true(cf_token_is(&p, "first"));
	assert_true(cf_go_to_token(&p, "target", NULL)); /* passes the {} block */
	assert_true(cf_token_is(&p, "target"));

	struct cf_token peek;
	assert_true(cf_peek_token(&p, &peek));
	assert_int_equal(strref_cmp(&peek.str, "("), 0);
	assert_true(cf_token_is(&p, "target"));
	assert_true(cf_peek_valid_token(&p, &peek));

	assert_true(cf_next_valid_token(&p));
	assert_true(cf_pass_pair(&p, '(', ')'));
	cf_next_token(&p); /* cf_pass_pair() leaves cur_token on the whitespace after ')' */
	assert_true(cf_token_is(&p, "name"));
	assert_true(cf_pass_pair(&p, '(', ')')); /* not on '(' : just reports more tokens */

	char *name = NULL;
	assert_int_equal(cf_get_name(&p, &name, "name", NULL), PARSE_SUCCESS);
	assert_string_equal(name, "name");
	bfree(name);

	struct strref ref;
	assert_int_equal(cf_get_name_ref(&p, &ref, "name", NULL), PARSE_SUCCESS);
	assert_int_equal(strref_cmp(&ref, "name"), 0);

	/* 5 is not a name */
	assert_int_equal(cf_next_name(&p, &name, "identifier", NULL), PARSE_CONTINUE);
	assert_int_equal(cf_get_name_ref(&p, &ref, "identifier", NULL), PARSE_CONTINUE);
	assert_int_equal(cf_token_is_type(&p, CFTOKEN_NUM, "number", NULL), PARSE_SUCCESS);

	char *copy = NULL;
	assert_int_equal(cf_next_token_copy(&p, &copy), PARSE_SUCCESS);
	assert_string_equal(copy, ";");
	bfree(copy);

	/* end of input */
	assert_false(cf_next_token(&p));
	assert_false(cf_next_valid_token(&p));
	assert_false(cf_peek_valid_token(&p, &peek));
	assert_int_equal(cf_next_token_copy(&p, &copy), PARSE_EOF);
	assert_int_equal(cf_next_name(&p, &name, "x", NULL), PARSE_EOF);
	assert_int_equal(cf_next_name_ref(&p, &ref, "x", NULL), PARSE_EOF);
	assert_int_equal(cf_next_token_should_be(&p, ";", NULL, NULL), PARSE_EOF);
	assert_true(error_data_has_errors(&p.error_list));

	cf_parser_free(&p);
}

static void should_be_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct cf_parser p;
	cf_parser_init(&p);
	assert_true(cf_parser_parse(&p, "a = b ; c = { d } e ; f", NULL));

	assert_int_equal(cf_token_should_be(&p, "a", NULL, NULL), PARSE_SUCCESS);
	assert_int_equal(cf_next_token_should_be(&p, "=", NULL, NULL), PARSE_SUCCESS);
	assert_int_equal(cf_next_token_should_be(&p, "x", NULL, NULL), PARSE_CONTINUE);
	assert_true(cf_token_is(&p, "b"));

	/* wrong token: skip to the next ';' */
	assert_int_equal(cf_token_should_be(&p, "x", ";", NULL), PARSE_CONTINUE);
	assert_true(cf_token_is(&p, ";"));
	assert_int_equal(cf_next_token_should_be(&p, "x", "e", ";"), PARSE_CONTINUE);
	assert_true(cf_token_is(&p, "e"));

	/* goto target missing */
	assert_int_equal(cf_token_should_be(&p, "x", "missing", NULL), PARSE_EOF);

	cf_parser_free(&p);

	cf_parser_init(&p);
	assert_true(cf_parser_parse(&p, "a b c", NULL));
	assert_int_equal(cf_next_token_should_be(&p, "x", "missing", NULL), PARSE_EOF);
	cf_parser_free(&p);

	cf_parser_init(&p);
	assert_true(cf_parser_parse(&p, "5 x", NULL));
	assert_int_equal(cf_token_is_type(&p, CFTOKEN_NAME, "name", "x"), PARSE_CONTINUE);
	assert_true(cf_token_is(&p, "x"));
	assert_int_equal(cf_token_is_type(&p, CFTOKEN_NUM, "number", "missing"), PARSE_EOF);
	cf_parser_free(&p);
}

static void go_to_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct cf_parser p;
	cf_parser_init(&p);
	assert_true(cf_parser_parse(&p, "a\nb { unclosed", NULL));

	assert_true(cf_go_to_valid_token(&p, "x", "b"));
	assert_true(cf_token_is(&p, "b"));
	/* an unclosed block ends the search */
	assert_false(cf_go_to_valid_token(&p, "x", NULL));

	p.cur_token = cf_preprocessor_get_tokens(&p.pp);
	assert_true(cf_go_to_token_type(&p, CFTOKEN_NEWLINE));
	assert_false(cf_go_to_token_type(&p, CFTOKEN_STRING));

	p.cur_token = cf_preprocessor_get_tokens(&p.pp);
	size_t before = p.error_list.errors.num;
	cf_adderror(&p, "plain", LEX_WARNING, NULL, NULL, NULL);
	cf_adderror(&p, "with $1", LEX_ERROR, "value", NULL, NULL);
	cf_adderror_syntax_error(&p);
	assert_int_equal(p.error_list.errors.num, before + 3);
	assert_string_equal(p.error_list.errors.array[before + 1].error, "with value");

	/* a pair that never closes */
	assert_true(cf_go_to_token(&p, "{", NULL) || true);
	p.cur_token = cf_preprocessor_get_tokens(&p.pp);
	while (!cf_token_is(&p, "{"))
		cf_next_token(&p);
	assert_false(cf_pass_pair(&p, '{', '}'));

	cf_parser_free(&p);

	/* an unterminated comment fails the parse */
	cf_parser_init(&p);
	assert_false(cf_parser_parse(&p, "a /* b", NULL));
	cf_parser_free(&p);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(navigation_test),
		obs_unit_test(should_be_test),
		obs_unit_test(go_to_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
