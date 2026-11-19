# Code the unit tests cannot reach

Regions of libobs that stay uncovered on purpose, because no test can reach them through the public API without
changing libobs. Each entry gives the file, the line range (as of the commit that added or last updated it) and the
reason. Line numbers drift; search for the quoted code when they do.

Reasons:

- **OOM**: allocation failure. `bmalloc()`/`brealloc()` never return NULL, they `bcrash()`.
- **OS failure**: a Win32 or CRT call that only fails when the OS is out of resources or broken.
- **Exit**: the code ends the process, so it cannot run inside a test process that reports results.
- **Once per process**: `bcrash()` refuses to run twice, so only one crash path per test executable is testable.
- **Dead**: no input reaches it.
- **Crash**: reaching it crashes libobs (a bug, listed under "Known bugs" below).

## util/

| File | Lines | Code | Reason |
| --- | --- | --- | --- |
| util/base.c | 58-63 | `def_crash_handler()`: `vfprintf(...); exit(0);` | Exit |
| util/base.c | 103-105 | `bcrash()`: `"Crashed in the crash handler"`, `exit(2)` | Exit (second `bcrash()`) |
| util/bmem.c | 111-113, 132-134 | `bmalloc()`/`brealloc()`: `os_oom(); bcrash("Out of memory ...")` | OOM |
| util/bmem.c | 125-127 | `brealloc()` with size 0: `os_breakpoint(); bcrash(...)` | Once per process (`test_util_bmem` uses its crash for `bmalloc(0)`) |
| util/bmem.c | `bmemdup()` | `if (size) memcpy(...)` false branch | Dead: `bmalloc(0)` already crashed |
| util/darray.h | `darray_move_item()`, `darray_swap()` | `if (!temp) bcrash(...)` after `malloc()` | OOM |
| util/dstr.c | 552, 574 | `dstr_vprintf()`/`dstr_vcatf()`: `if (len < 0) len = 4095;` | OS failure: `vsnprintf(NULL, 0, ...)` only fails on an encoding error the MSVC CRT does not produce for valid formats |
| util/dstr.c | 755, 778 | `dstr_to_upper()`/`dstr_to_lower()`: `if (!wstr) return;` | Dead: `dstr_to_wcs()` of a non-empty dstr always returns a string |
| util/dstr.h | 256 | `dstr_is_empty()`: `return true` for a non-NULL array with `*array == 0` | Dead: every dstr function frees instead of keeping an empty array |
| util/lexer.c | 168 | `valid_float_str()`: sign branch `return false` | Reachable only for signed exponents, which the function rejects earlier (see "Known bugs") |
| util/cf-lexer.c | 75-77 | octal escape in `cf_convert_from_escape_literal()` | Reachable, but wrong (see "Known bugs"); not asserted |
| util/cf-lexer.c | 591-593, 1126, 1157-1159 | macro call without `(` or left open at end of file | Crash |
| util/cf-lexer.c | 1178-1182 | zero-parameter macro call `M()` | Dead: `#define M()` is rejected, so no zero-parameter macro exists (see "Known bugs") |
| util/cf-lexer.c | `next_token()` | `preprocessor == false` loop condition `SPACETAB && NEWLINE` | Dead: the condition can never be true; every caller passes `true` anyway |
| util/cf-lexer.c | 1350 | `cf_preprocess()`: `if (!token) return false;` | Dead: `cf_lexer_lex()` always leaves at least the end token |
| util/config-file.c | 82-84, 281-283, 309-311 | `pthread_mutex_init_recursive()` failure | OS failure |
| util/config-file.c | 349-351 | `config_save()`: `os_fopen()` fails | Reachable with a read-only file; not covered yet |
| util/config-file.c | 380, 384 | `fwrite()` failure | OS failure (disk full) |
| util/config-file.c | 424-429 | `config_save_safe()`: temporary file cannot be written | Reachable with a read-only directory; not covered yet |
| util/config-file.c | 439 | `os_safe_replace()` failure | OS failure |
| util/pipe-windows.c | 40-41, 101-106, 112-118 | `CreatePipe()`/`SetHandleInformation()` failure | OS failure |
| util/pipe-windows.c | 280 | `os_process_pipe_read_err()` when `handle_err` is NULL | Dead: always set on success |
| util/buffered-file-serializer.c | 69-72 | `bmalloc()` returned NULL | Dead: `bmalloc()` never returns NULL |
| util/buffered-file-serializer.c | 182-187, 215, 262, 317 | `fwrite()` failure and the `output_error` paths it enables | OS failure (disk full) |
| util/buffered-file-serializer.c | 273-279 | writer waits for the I/O thread | Timing: only when the I/O thread falls behind; flaky to force |
| util/serializer.h | 61 | `serialize()` read path | Reachable with an input serializer; not covered yet |
| util/utf8.c | 37 | `wchar_to_utf8()` with `insize == 0` | Reachable only through `dstr_from_wcs()`, not covered yet |
| util/profiler.c | `profiler_free()` | freeing after a start/snapshot/stop cycle | Crash |

Not yet covered and not tested on purpose (threading-level, may be replaced):

- util/threading-windows.c, util/threading-windows.h, util/threading.h
- util/task.c, util/spsc-fifo.h

Not yet covered, reachable, no test yet:

- util/source-profiler.c (needs sources rendering on the null graphics module)
- util/windows/window-helpers.c, util/windows/device-enum.c, util/windows/obfuscate.c, util/windows/win-version.h
- util/util_uint64.h (the `util_mul_div64()` portable path)

## Known bugs

Found while writing the util/ tests and not fixed, because the fix is not trivially small or changes behaviour that
callers may depend on.

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
