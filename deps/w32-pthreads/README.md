# w32-pthreads (native)

`w32-pthreads.dll` provides POSIX threads on Windows for libobs, the frontend
and plugins. It used to be pthreads-win32 2.9.1, which emulates most of
POSIX on top of Win32 events and its own lock implementations. This is a
native reimplementation of the same library:

- **Same ABI.** `pthread.h`, `sched.h` and `semaphore.h` are the unchanged
  pthreads-win32 2.9.1 headers, and the DLL exports the same 120 symbols
  under the same name. Plugins built against upstream OBS Studio keep loading
  and work without a rebuild. `pthread_t` is still the 16-byte
  `{pointer, reuse counter}` pair, every other handle is still one pointer,
  and the `PTHREAD_*_INITIALIZER` sentinels keep their meaning.
- **Native primitives.** Mutexes and rwlocks are `SRWLOCK`, condition
  variables are `CONDITION_VARIABLE`, `pthread_once` is `INIT_ONCE`, barriers
  are `SYNCHRONIZATION_BARRIER`, keys are TLS slots, semaphores are kernel
  semaphores, threads come from `_beginthreadex`. Requires Windows 10.

## Behavior differences from pthreads-win32

- **Asynchronous cancellation** is treated as deferred, except when a thread
  cancels itself. Deferred cancellation works as before, and
  `pthread_cancel()` still wakes a target blocked in `pthread_cond_wait`,
  `pthread_join`, `sem_wait`/`sem_timedwait` or `pthread_delay_np`.
- **Robust mutexes** are not supported: `pthread_mutexattr_setrobust(...,
  PTHREAD_MUTEX_ROBUST)` returns `ENOTSUP`.
- **Timed locks** (`pthread_mutex_timedlock`, `pthread_rwlock_timed*lock`)
  poll, because `SRWLOCK` has no timed acquire. They are rarely used.
- **`sem_getvalue`** never reports a negative value (number of waiters).
- **Process-shared objects** return `ENOSYS`, as before.

Kept on purpose, because existing code depends on it: locking a zeroed or
destroyed mutex returns `EINVAL` instead of crashing, a normal mutex may be
unlocked by a thread other than its owner, and unlocking an unlocked normal
mutex is a no-op.

## Tests

`tests/` holds the original pthreads-win32 conformance suite plus
`tests/knurbs-compat.c`, which checks the behaviors above and prints a few
performance numbers. Six suite tests (`condvar2`, `condvar2_1`, `condvar3_1`,
`condvar3_2`, `context1`, `sizes`) inspect pthreads-win32 internals and do not
apply to this implementation.

## License

LGPL 2.1 or later, as before (see `COPYING.LIB`).
