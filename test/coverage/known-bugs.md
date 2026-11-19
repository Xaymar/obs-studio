# Known libobs bugs

Bugs found while writing the unit tests and not fixed yet: the fix is not trivially small, or it changes behaviour
callers may depend on. Each entry names the file and how to trigger it. Bugs that were small enough were fixed in
their own commits with a regression test and are not listed here.

## util/

| File | Bug |
| --- | --- |
| util/cf-lexer.c | A function-like macro used without `(` (`#define M(a, b) a b` then `M 2`) crashes the preprocessor with an access violation. |
| util/cf-lexer.c | A macro call left open at end of file (`#define M(a) a` then `M(1, 2`) crashes the preprocessor: the token pointer steps past the end token. |
| util/cf-lexer.c | `#define M()` (zero parameters) is rejected with "Expected identifier", so zero-parameter function macros cannot be defined. |
| util/cf-lexer.c | Octal escapes in `cf_literal_to_str()` are decoded from the wrong position: the first digit is consumed by the `switch`, so `"\101"` becomes `"\x01"` instead of `"A"`. `"\0"` followed by digits is also taken as a lone NUL. |
| util/cf-lexer.c | `cf_preprocessor_add_def()` reports a redefinition through `pp->lex` and `pp->ed`, which are NULL until `cf_preprocess()` runs, so redefining before preprocessing crashes. |
| util/cf-lexer.c | Errors in `#include`d files are reported with row/column computed against the including file's text. |
| util/lexer.c | `valid_float_str()` rejects every signed exponent (`1e-3`, `1e+3`): `found_num` is reset at `e`, and the sign branch requires it. |
| util/dstr.c | `dstr_ncopy()` copies `len + 1` bytes from `array`, reading one byte past a non-terminated strref. |
| util/config-file.c | A section header without `]` (`[Broken`) still creates a section; the parse result is ignored. |
| util/config-file.c | Duplicate keys or sections are added to the hash twice instead of replacing the first. |
| util/text-lookup.c | Whitespace around `=` is not skipped: `Key = Value` stores `" "` as the value. |
| util/array-serializer.c | `get_pos` returns the array size instead of the write position after a seek. |
| util/array-serializer.c, util/buffered-file-serializer.c | `SERIALIZE_SEEK_END` moves backwards by `offset`; the buffered writer counts from its current position rather than the end. The file serializer uses `fseek()` semantics. |
| util/platform-windows.c | `os_cpu_usage_info_query()` divides by zero (returns NaN) when called within the same clock tick as `os_cpu_usage_info_start()` or a previous query. |
| util/platform.c | `os_get_path_extension(NULL)` crashes, unlike most `os_*` path helpers. |
| util/profiler.c | `profiler_free()` crashes (access violation) after `profiler_start()`, a snapshot and `profiler_stop()`. |
| util/profiler.c | A snapshot reports fewer root calls than were made in `test_util_profiler`; not investigated yet. |
| util/profiler.c | `profiler_snapshot_filter_roots(NULL, ...)` crashes, unlike every other snapshot accessor. |
| obs.c | `obs_get_video_info()` (and so `os_generate_formatted_filename()`) dereferences `obs` and crashes before `obs_startup()`. |
