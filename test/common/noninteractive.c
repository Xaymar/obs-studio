/* Linked into every test program: on Windows, failures (abort(), failed asserts, CRT reports, crashes,
 * missing files) are written to stderr instead of opening a dialog that waits for someone to click it. */

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#include <stdlib.h>

static void noninteractive_init(void)
{
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
}

/* run before main(), through the CRT's initializer table */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*noninteractive_init_ptr)(void) = noninteractive_init;
#endif
