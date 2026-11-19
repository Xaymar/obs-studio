#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <stdio.h>
#include <string.h>

#include <util/bmem.h>
#include <util/cf-lexer.h>
#include <util/dstr.h>
#include <util/platform.h>

/* Joins the significant tokens (no whitespace or newlines) with single spaces. */
static char *join_tokens(const struct cf_token *token)
{
	struct dstr out = {0};
	for (; token->type != CFTOKEN_NONE; token++) {
		if (token->type == CFTOKEN_SPACETAB || token->type == CFTOKEN_NEWLINE)
			continue;
		if (out.len)
			dstr_cat_ch(&out, ' ');
		dstr_ncat(&out, token->str.array, token->str.len);
	}
	return out.array ? out.array : bstrdup("");
}

struct pp_result {
	struct cf_lexer lex;
	struct cf_preprocessor pp;
	struct error_data ed;
	char *text;
};

static bool preprocess(struct pp_result *r, const char *source, const char *file)
{
	cf_lexer_init(&r->lex);
	cf_preprocessor_init(&r->pp);
	error_data_init(&r->ed);
	assert_true(cf_lexer_lex(&r->lex, source, file));
	bool ok = cf_preprocess(&r->pp, &r->lex, &r->ed);
	r->text = join_tokens(cf_preprocessor_get_tokens(&r->pp));
	return ok;
}

static void pp_result_free(struct pp_result *r)
{
	bfree(r->text);
	error_data_free(&r->ed);
	cf_preprocessor_free(&r->pp);
	cf_lexer_free(&r->lex);
}

static void assert_no_errors(struct pp_result *r)
{
	if (r->ed.errors.num) {
		char *text = error_data_buildstring(&r->ed);
		print_error("%s", text);
		bfree(text);
	}
	assert_int_equal(r->ed.errors.num, 0);
}

static bool has_error(struct pp_result *r, const char *needle)
{
	for (size_t i = 0; i < r->ed.errors.num; i++) {
		if (strstr(r->ed.errors.array[i].error, needle))
			return true;
	}
	return false;
}

static void lex_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct cf_lexer lex;
	cf_lexer_init(&lex);
	assert_false(cf_lexer_lex(&lex, "", NULL));
	assert_false(cf_lexer_lex(&lex, NULL, NULL));

	assert_true(cf_lexer_lex(&lex, "float4 x_1 = 1.5f + .5; // tail\n\"str\\\"ing\" 'c'", "test"));
	struct cf_token *t = cf_lexer_get_tokens(&lex);
	assert_int_equal(t[0].type, CFTOKEN_NAME);
	assert_int_equal(strref_cmp(&t[0].str, "float4"), 0);
	assert_int_equal(t[1].type, CFTOKEN_SPACETAB);
	assert_int_equal(strref_cmp(&t[2].str, "x_1"), 0);
	assert_int_equal(t[4].type, CFTOKEN_OTHER);
	assert_int_equal(t[6].type, CFTOKEN_NUM);
	assert_int_equal(strref_cmp(&t[6].str, "1.5f"), 0);

	char *joined = join_tokens(t);
	assert_string_equal(joined, "float4 x_1 = 1.5f + .5 ; \"str\\\"ing\" 'c'");
	bfree(joined);

	/* block comments, splices and an unterminated comment */
	/* an unterminated block comment makes cf_lexer_lex() fail but still produces tokens */
	assert_false(cf_lexer_lex(&lex, "a/* x * y */b \\\nc /\\\n/ splice comment\nd /* open", NULL));
	assert_true(lex.unexpected_eof);
	joined = join_tokens(cf_lexer_get_tokens(&lex));
	assert_string_equal(joined, "a b c d");
	bfree(joined);

	/* unterminated string stops at the newline */
	assert_true(cf_lexer_lex(&lex, "\"open\nnext", NULL));
	t = cf_lexer_get_tokens(&lex);
	assert_int_equal(t[0].type, CFTOKEN_STRING);
	assert_int_equal(strref_cmp(&t[0].str, "\"open"), 0);

	cf_lexer_free(&lex);
}

static void literal_test(void **state)
{
	UNUSED_PARAMETER(state);

	char *str = cf_literal_to_str("\"a\\'\\\"\\?\\\\\\0b\"", 0);
	assert_memory_equal(str, "a'\"?\\\0b", 8); /* also proves the closing quote is gone */
	bfree(str);

	str = cf_literal_to_str("'\\a\\b\\f\\n\\r\\t\\v\\x41\\X42'", 0);
	assert_string_equal(str, "\a\b\f\n\r\t\vAB");
	bfree(str);

	/* Regression: octal escapes were decoded from the second digit, and "\0" followed by digits was a lone NUL */
	str = cf_literal_to_str("\"\\101\\7\\0123\\08\"", 0);
	assert_memory_equal(str, "A\a\n3\0" "8", 7);
	bfree(str);

	/* hex escapes take at most two digits */
	str = cf_literal_to_str("\"\\x41BC\\x4g\\xz\"", 0);
	assert_string_equal(str, "ABC\x04gz");
	bfree(str);

	str = cf_literal_to_str("\"abc\"xyz", 5);
	assert_string_equal(str, "abc");
	bfree(str);

	/* unknown escapes are dropped */
	str = cf_literal_to_str("\"a\\qb\"", 0);
	assert_string_equal(str, "ab");
	bfree(str);

	assert_null(cf_literal_to_str("\"", 0));
	assert_null(cf_literal_to_str("\"abc'", 0));
	assert_null(cf_literal_to_str("(abc(", 0));
}

static void define_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct pp_result r;
	assert_true(preprocess(&r,
			       "#define VALUE 42\n"
			       "#define ADD(a, b) ((a) + (b))\n"
			       "#define SPLIT one \\\n two\n"
			       "x = VALUE; y = ADD(1, VALUE); SPLIT\n"
			       "#undef VALUE\n"
			       "z = VALUE;\n",
			       NULL));
	assert_string_equal(r.text, "x = 42 ; y = ( ( 1 ) + ( 42 ) ) ; one two z = VALUE ;");
	assert_no_errors(&r);
	pp_result_free(&r);

	/* malformed directives */
	const char *bad[] = {
		"#define\n",
		"#define 5\n",
		"#define BAD(1)\n",
		"#define BAD2(a b)\n",
		"#undef\n",
		"#undef 5\n",
		"#define M(a, b) a b\nM(1)\n",
	};
	const char *expect[] = {
		"Expected identifier",
		"Expected identifier",
		"Expected identifier",
		"Expected ',' or ')'",
		"Expected identifier",
		"Expected identifier",
		"Mismatching number of macro parameters",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		assert_true(preprocess(&r, bad[i], NULL));
		if (!has_error(&r, expect[i])) {
			char *errors = error_data_buildstring(&r.ed);
			fail_msg("case %zu: expected '%s', got:\n%s", i, expect[i], errors ? errors : "(none)");
		}
		pp_result_free(&r);
	}

	/* Regression: zero-parameter function-like macros were rejected with "Expected identifier" */
	assert_true(preprocess(&r, "#define Z() zero\n#define W( ) [ ]\na Z() W() b\n", NULL));
	assert_string_equal(r.text, "a zero [ ] b");
	assert_no_errors(&r);
	pp_result_free(&r);
	assert_true(preprocess(&r, "#define Z() zero\nZ(1)\n", NULL));
	assert_true(has_error(&r, "Mismatching number of macro parameters"));
	pp_result_free(&r);
	assert_true(preprocess(&r, "#define Z(a,) a\n", NULL));
	assert_true(has_error(&r, "Expected identifier"));
	pp_result_free(&r);

	/* commas inside nested parentheses belong to the argument */
	assert_true(preprocess(&r, "#define M(a, b) [a|b]\nM(f(1, 2), 3)\n", NULL));
	assert_string_equal(r.text, "[ f ( 1 , 2 ) | 3 ]");
	pp_result_free(&r);

	/* Regression: a macro call left open at end of file stepped past the end token, so the output ended in
	 * freed memory instead of a CFTOKEN_NONE terminator */
	assert_true(preprocess(&r, "#define M(a) a\nM(1, 2", NULL));
	assert_true(has_error(&r, "Unexpected end of file"));
	const struct cf_token *tok = cf_preprocessor_get_tokens(&r.pp);
	size_t count = 0;
	while (tok[count].type != CFTOKEN_NONE)
		count++;
	assert_int_equal(count + 1, r.pp.tokens.num);
	pp_result_free(&r);

	/* a function-like macro used without '(' is reported, not expanded */
	assert_true(preprocess(&r, "#define M(a, b) a b\nM 2\n", NULL));
	assert_true(has_error(&r, "Expected '('"));
	pp_result_free(&r);
}

static void conditional_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct pp_result r;
	assert_true(preprocess(&r,
			       "#define ON 1\n"
			       "#define TWO 2\n"
			       "#define ALIAS TWO\n"
			       "#ifdef ON\na\n#else\nb\n#endif\n"
			       "#ifndef ON\nc\n#else\nd\n#endif\n"
			       "#ifdef OFF\ne\n#elif TWO == 2\nf\n#endif\n"
			       "#ifdef ON\ng\n#elif TWO == 2\nh\n#endif\n"
			       "#if ON\ni\n#endif\n"
			       "#if ALIAS >= 2\nj\n#endif\n"
			       "#if TWO <= 1\nk\n#else\nl\n#endif\n"
			       "#if TWO != 2\nm\n#elif 1 < 2\nn\n#endif\n"
			       "#if 3 > 2\no\n#elif 1\np\n#endif\n"
			       "#ifdef OFF\n#ifdef ON\nq\n#endif\n#include \"missing.h\"\n#endif\n",
			       NULL));
	assert_string_equal(r.text, "a d f g i j l n o");
	assert_no_errors(&r);
	pp_result_free(&r);

	assert_true(preprocess(&r,
			       "#if\n#endif\n"
			       "#if 1 2\n#endif\n"
			       "#if 1 ~ 2\n#endif\n"
			       "#if 1 == x\n#endif\n"
			       "#ifdef 5\n#endif\n"
			       "#endif\n"
			       "#else\n",
			       NULL));
	assert_true(has_error(&r, "Expected identifier or number"));
	assert_true(has_error(&r, "Expected comparator"));
	assert_true(has_error(&r, "outside of #if/#ifdef/#ifndef block"));
	pp_result_free(&r);

	assert_true(preprocess(&r, "#ifdef X\nabc", NULL));
	assert_true(has_error(&r, "Unexpected end of file before #endif"));
	pp_result_free(&r);

	assert_true(preprocess(&r, "#define A 1 junk\n#if A\nx\n#endif\n#ifdef A extra\n#endif\n", NULL));
	pp_result_free(&r);
}

static void include_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_mkdir("inc");
	FILE *f = os_fopen("inc/header.h", "wb");
	assert_non_null(f);
	fputs("#define FROM_HEADER 7\nheader_token\n", f);
	fclose(f);

	struct pp_result r;
	assert_true(preprocess(&r,
			       "#include \"header.h\"\n"
			       "#include \"header.h\"\n"
			       "#include <system.h>\n"
			       "v = FROM_HEADER;\n",
			       "inc/main.c"));
	assert_string_equal(r.text, "header_token header_token v = 7 ;");
	pp_result_free(&r);

	/* Regression: errors in an included file were positioned against the including file's text */
	f = os_fopen("inc/bad.h", "wb");
	assert_non_null(f);
	fputs("ok\n  #define 5\n", f);
	fclose(f);
	/* Regression: tokens of an included file pointed at a lexer on the stack, which the second include used */
	assert_true(preprocess(&r, "a\nb\nc\n#include \"bad.h\"\n#include \"bad.h\"\n", "inc/main.c"));
	assert_int_equal(r.ed.errors.num, 2);
	for (size_t i = 0; i < 2; i++) {
		assert_string_equal(r.ed.errors.array[i].file, "inc/bad.h");
		assert_int_equal(r.ed.errors.array[i].row, 2);
		assert_int_equal(r.ed.errors.array[i].column, 11);
	}
	for (struct cf_token *t = cf_preprocessor_get_tokens(&r.pp); t->type != CFTOKEN_NONE; t++) {
		if (strref_cmp(&t->str, "ok") == 0)
			assert_string_equal(t->lex->file, "inc/bad.h");
	}
	pp_result_free(&r);
	os_unlink("inc/bad.h");

	/* errors do not fail cf_preprocess(); only an unterminated comment does */
	assert_true(preprocess(&r, "#include \"nope.h\"\n#include 5\n#include 'x'\n", NULL));
	assert_true(has_error(&r, "Could not open file"));
	assert_true(has_error(&r, "Expected string"));
	assert_true(has_error(&r, "Invalid or incomplete string"));
	pp_result_free(&r);

	os_unlink("inc/header.h");
	os_rmdir("inc");
}

static void api_def_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct cf_lexer def_lex;
	cf_lexer_init(&def_lex);
	assert_true(cf_lexer_lex(&def_lex, "NAME 5", NULL));

	struct cf_preprocessor pp;
	cf_preprocessor_init(&pp);
	/* Regression: a redefinition before cf_preprocess() crashed, pp->lex and pp->ed were still NULL */
	struct cf_def early;
	cf_def_init(&early);
	cf_token_copy(&early.name, cf_lexer_get_tokens(&def_lex));
	cf_preprocessor_add_def(&pp, &early);
	struct cf_def early2;
	cf_def_init(&early2);
	cf_token_copy(&early2.name, cf_lexer_get_tokens(&def_lex));
	cf_preprocessor_add_def(&pp, &early2);
	cf_preprocessor_remove_def(&pp, "NAME");

	/* with an error list the redefinition warnings are recorded */
	struct error_data ed;
	error_data_init(&ed);
	pp.lex = &def_lex;
	pp.ed = &ed;
	cf_preprocessor_add_sys_include_dir(&pp, "sys");
	cf_preprocessor_add_sys_include_dir(&pp, NULL);

	struct cf_def def;
	cf_def_init(&def);
	cf_token_copy(&def.name, cf_lexer_get_tokens(&def_lex));
	cf_def_addtoken(&def, cf_lexer_get_tokens(&def_lex) + 2);
	cf_preprocessor_add_def(&pp, &def);

	/* Redefining replaces the old definition */
	struct cf_def again;
	cf_def_init(&again);
	cf_token_copy(&again.name, cf_lexer_get_tokens(&def_lex));
	cf_def_addparam(&again, cf_lexer_get_tokens(&def_lex));
	assert_non_null(cf_def_getparam(&again, 0));
	cf_preprocessor_add_def(&pp, &again);

	cf_preprocessor_remove_def(&pp, "NAME");
	cf_preprocessor_remove_def(&pp, "NOT_THERE");

	assert_int_equal(error_data_type_count(&ed, LEX_WARNING), 2);

	cf_preprocessor_free(&pp);
	error_data_free(&ed);
	cf_lexer_free(&def_lex);

	struct cf_token a, b;
	cf_token_clear(&a);
	cf_token_clear(&b);
	strref_set(&a.str, "ab", 1);
	strref_set(&b.str, "b", 1);
	cf_token_add(&a, &b);
	assert_int_equal(a.str.len, 2);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(lex_test),         obs_unit_test(literal_test), obs_unit_test(define_test),
		obs_unit_test(conditional_test), obs_unit_test(include_test), obs_unit_test(api_def_test),
	};

	int ret = cmocka_run_group_tests(tests, NULL, NULL);
	return ret ? ret : (bnum_allocs() != 0);
}
