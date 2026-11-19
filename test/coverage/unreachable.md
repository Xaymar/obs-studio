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
- **Crash**: reaching it crashes libobs, so no test can run it (see known-bugs.md).

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
| util/lexer.c | 168 | `valid_float_str()`: sign branch `return false` | Reachable only for signed exponents, which the function rejects earlier (see known-bugs.md) |
| util/cf-lexer.c | 75-77 | octal escape in `cf_convert_from_escape_literal()` | Reachable, but wrong (see known-bugs.md); not asserted |
| util/cf-lexer.c | 591-593, 1126, 1157-1159 | macro call without `(` or left open at end of file | Crash |
| util/cf-lexer.c | 1178-1182 | zero-parameter macro call `M()` | Dead: `#define M()` is rejected, so no zero-parameter macro exists (see known-bugs.md) |
| util/cf-lexer.c | `next_token()` | `preprocessor == false` loop condition `SPACETAB && NEWLINE` | Dead: the condition can never be true; every caller passes `true` anyway |
| util/cf-lexer.c | 1350 | `cf_preprocess()`: `if (!token) return false;` | Dead: `cf_lexer_lex()` always leaves at least the end token |
| util/config-file.c | 82-84, 281-283, 309-311 | `pthread_mutex_init_recursive()` failure | OS failure |
| util/config-file.c | 380, 384 | `fwrite()` failure | OS failure (disk full) |
| util/config-file.c | 439 | `os_safe_replace()` failure | OS failure |
| util/pipe-windows.c | 40-41, 101-106, 112-118 | `CreatePipe()`/`SetHandleInformation()` failure | OS failure |
| util/pipe-windows.c | 280 | `os_process_pipe_read_err()` when `handle_err` is NULL | Dead: always set on success |
| util/buffered-file-serializer.c | 69-72 | `bmalloc()` returned NULL | Dead: `bmalloc()` never returns NULL |
| util/buffered-file-serializer.c | 182-187, 215, 262, 317 | `fwrite()` failure and the `output_error` paths it enables | OS failure (disk full) |
| util/buffered-file-serializer.c | 273-279 | writer waits for the I/O thread | Timing: only when the I/O thread falls behind; flaky to force |
| util/profiler.c | `profiler_free()` | freeing after a start/snapshot/stop cycle | Crash |

Not yet covered and not tested on purpose (threading-level, may be replaced):

- util/threading-windows.c, util/threading-windows.h, util/threading.h
- util/task.c, util/spsc-fifo.h

Reachable, but no test yet (not unreachable; tracked here until covered):

- util/source-profiler.c (needs sources rendering on the null graphics module)
- util/windows/window-helpers.c, util/windows/device-enum.c, util/windows/obfuscate.c, util/windows/win-version.h
- util/util_uint64.h (the `util_mul_div64()` portable path)
- util/config-file.c: 349-351: `config_save()`: `os_fopen()` fails
- util/config-file.c: 424-429: `config_save_safe()`: temporary file cannot be written
- util/serializer.h: 61: `serialize()` read path
- util/utf8.c: 37: `wchar_to_utf8()` with `insize == 0`
