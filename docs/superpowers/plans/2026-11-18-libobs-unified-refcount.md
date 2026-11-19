# Unified libobs Refcounting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace every CPU-side refcounting scheme in libobs with one embedded 32-byte header and one exported `obs_mem_*` API, keeping every existing exported function and behaviour.

**Architecture:** `struct obs_mem_header` is the first member of every refcounted `obs_*` struct; for context types it is the first member of `obs_context_data`. Strong counts live in the header. Weak refs use a lazily allocated `struct obs_mem_weak` with a pin counter for safe weak→strong upgrades. Per-type functions become one-line exported wrappers.

**Tech Stack:** C17, libobs atomics (`util/threading-*.h`), CMake, Windows (MSVC) and POSIX.

**Spec:** `docs/superpowers/specs/2026-11-18-libobs-unified-refcount-design.md`. Read it first; this plan does not repeat its rationale.

## Global Constraints

- Branch `33.0/libobs-refcount`. Never push or open PRs. A branch push to `origin` happens only when the user asks.
- No exported symbol is removed or changes signature. Per-type functions stay real functions, never macros (obs.hpp takes them as template function pointers).
- Every count starts at 1; the object is destroyed when it reaches 0.
- `sizeof(struct obs_mem_header) == 32`, with 32-byte alignment (`bmalloc` `ALIGNMENT 32`).
- Allocation stays `bzalloc`/`bfree`/`brealloc`.
- Each typed release keeps exactly its current NULL and `!obs` checks: today only `obs_source_release`, `obs_canvas_release` and `obs_object_release` check `!obs`, and `obs_object_release` checks it before the NULL check.
- Sources are still destroyed through `obs_source_destroy` → `obs_source_destroy_defer` on `obs->destruction_task_thread`.
- `obs_mem_*` is not added to the SWIG `.i` files.
- Code style: run `clang-format` (the repo's `.clang-format`) on every touched C file. Commits are prefixed `libobs:` and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A weak→strong upgrade racing the last release** on another thread. It must return either a valid strong ref or NULL, never touch freed memory. Pinned by `test_upgrade_race` (Task 1).
2. **`obs_*_get_weak_*` called from a `"destroy"` signal handler or `info.destroy`**, i.e. after the last release. It must return an already-expired weak ref that frees cleanly. Pinned by `test_get_weak_while_dying` (Task 1) and `test_source_weak_in_destroy_signal` (Task 4).
3. **A weak ref outliving a forced destroy** (`obs_free_data` at shutdown, or a direct `obs_missing_file_destroy`). It must report expired and upgrade to NULL. Pinned by `test_finalize_forced` (Task 1).
4. **A plugin dropping the last ref of a display, fader, volmeter or properties from inside that object's own callback.** No use-after-free or deadlock. Pinned by `test_fader_release_in_callback` (Task 5); the display case is covered by the manual check in Task 6.
5. **The `obs_data_item_release(**)` quirk** (it NULLs `*item` only when the item was destroyed) and `obs_data_item_next` iteration. Pinned by `test_data_item_release_quirk` (Task 2).

---

## File Structure

- Create `libobs/obs-mem.h`: public, installed header with the header struct, the weak typedef and the API.
- Create `libobs/obs-mem.c`: the implementation.
- Modify `libobs/util/threading-windows.h` and `libobs/util/threading-posix.h`: add pointer atomics.
- Modify `libobs/CMakeLists.txt`: add the sources (near line 59) and the public header (near line 325).
- Create `test/test-obs-mem/CMakeLists.txt` and `test/test-obs-mem/test-obs-mem.c`: a standalone assert-based test that links `OBS::libobs`. The cmocka tests are not wired into the top-level build on this branch, so this test doesn't use cmocka.
- Modify the top-level `CMakeLists.txt`: add `enable_testing()` and `add_subdirectory(test/test-obs-mem)` next to `test/test-input`.
- Create `docs/sphinx/reference-libobs-obs-mem.rst`, and link it from `docs/sphinx/reference-libobs-util.rst`'s toctree or the closest libobs reference index.
- Per-type conversions modify:
  - `obs-data.c`, `obs-scene.c`, `obs-scene.h`, `obs-missing-files.c`
  - `obs-internal.h`, `obs.c`, `obs-source.c`, `obs-output.c`, `obs-encoder.c`, `obs-service.c`, `obs-canvas.c`
  - `obs-display.c`, `obs-video.c`, `obs-view.c`, `obs-audio-controls.c`, `obs-properties.c`

---

### Task 1: Core `obs_mem` API, pointer atomics and its test

**Files:**
- Create: `libobs/obs-mem.h`, `libobs/obs-mem.c`, `test/test-obs-mem/CMakeLists.txt`, `test/test-obs-mem/test-obs-mem.c`, `docs/sphinx/reference-libobs-obs-mem.rst`
- Modify: `libobs/util/threading-windows.h`, `libobs/util/threading-posix.h`, `libobs/CMakeLists.txt`, `CMakeLists.txt`, `docs/sphinx/reference-libobs-util.rst`

**Interfaces:**
- Produces, in `util/threading-*.h`:
  - `static inline void *os_atomic_load_ptr(void *const volatile *ptr)`
  - `static inline void *os_atomic_set_ptr(void *volatile *ptr, void *val)`, which returns the old value
  - `static inline bool os_atomic_compare_exchange_ptr(void *volatile *ptr, void **old_val, void *new_val)`, with the same semantics as `os_atomic_compare_exchange_long`
- Produces, in `obs-mem.h`:

```c
typedef void (*obs_mem_destroy_cb)(void *obj);
typedef struct obs_mem_weak obs_mem_weak_t;

struct obs_mem_header {             /* 32 bytes, 32-byte aligned, first member */
	volatile long refs;
	struct obs_mem_weak *volatile weak; /* NULL, a block, or OBS_MEM_WEAK_DYING */
	obs_mem_destroy_cb destroy;
	volatile long flags;
};

EXPORT void obs_mem_init(void *obj, obs_mem_destroy_cb destroy); /* refs = 1, weak = NULL */
EXPORT void *obs_mem_acquire(void *obj);         /* unconditional ++; returns obj; NULL-safe */
EXPORT void *obs_mem_get_ref(void *obj);         /* ++ only if refs > 0, else NULL */
EXPORT bool obs_mem_release(void *obj);          /* true if this call destroyed obj; NULL → false */
EXPORT obs_mem_weak_t *obs_mem_get_weak(void *obj);
EXPORT void obs_mem_weak_acquire(obs_mem_weak_t *weak);
EXPORT void obs_mem_weak_release(obs_mem_weak_t *weak);
EXPORT void *obs_mem_weak_get_strong(obs_mem_weak_t *weak);
EXPORT bool obs_mem_weak_expired(obs_mem_weak_t *weak); /* NULL → true */
EXPORT bool obs_mem_weak_references(obs_mem_weak_t *weak, void *obj);
EXPORT void obs_mem_finalize(void *obj);         /* call right before the final bfree */
```

- `struct obs_mem_weak { volatile long weak_refs; volatile long pins; volatile long dead; void *object; }` is private to `obs-mem.c`.
- `OBS_MEM_WEAK_DYING` is `((struct obs_mem_weak *)(uintptr_t)1)` and is private to `obs-mem.c`.

- [ ] **Step 1: Write the failing test** `test/test-obs-mem/test-obs-mem.c`. It uses a `struct test_obj { struct obs_mem_header hdr; int destroyed; obs_mem_weak_t *weak_in_dtor; }` whose destroy callback sets `destroyed` and, when asked, calls `obs_mem_get_weak` on itself. It uses `assert()` and has a `main()` that runs:
  - `test_header_layout`: `sizeof(struct obs_mem_header) == 32` and `offsetof(struct test_obj, hdr) == 0`.
  - `test_counting`: after `init`, `acquire` then `release` returns false; the next `release` returns true and `destroyed == 1`; `obs_mem_release(NULL) == false`.
  - `test_get_ref_dying`: inside the destroy callback, `obs_mem_get_ref(obj) == NULL`.
  - `test_weak_basic`: `get_weak` twice returns the same pointer. While alive, `weak_get_strong == obj` and `weak_expired == false`. After the last release, `weak_expired == true`, `weak_get_strong == NULL` and `weak_references(weak, obj) == true`. Releasing the weak ref twice (one per `get_weak`) frees it; run under ASan or the CRT debug heap where available.
  - `test_get_weak_while_dying`: the destroy callback calls `obs_mem_get_weak`. The returned block reports `expired == true` and `get_strong == NULL`, and releasing it once frees it.
  - `test_finalize_forced`: hold a weak ref, then call `obs_mem_finalize(obj)` while `refs == 1` (simulating `obs_free_data`). Afterwards `weak_expired == true` and `weak_get_strong == NULL`.
  - `test_upgrade_race`: 4 threads × 100000 iterations. Each iteration creates an object with a weak ref; thread A releases the last strong ref while thread B calls `weak_get_strong` and, if it got a ref, releases it. Assert `destroyed == 1` exactly once per object and that no strong ref is returned after `destroyed` was set.
- [ ] **Step 2: Wire the test into CMake.** `add_executable(test-obs-mem test-obs-mem.c)`, `target_link_libraries(test-obs-mem PRIVATE OBS::libobs)`, `add_test(NAME test-obs-mem COMMAND test-obs-mem)`, and `enable_testing()` in the top-level `CMakeLists.txt`. Configure with `cmake --preset windows-x64` and build with `cmake --build --preset windows-x64 --target test-obs-mem`. Expected: FAIL to compile (`obs-mem.h` not found).
- [ ] **Step 3: Add the pointer atomics** to both threading headers: `_InterlockedExchangePointer`, `_InterlockedCompareExchangePointer` and a volatile read on Windows; `__atomic_*_n` with `__ATOMIC_SEQ_CST` on POSIX.
- [ ] **Step 4: Implement `obs-mem.h` and `obs-mem.c`** exactly as in the spec's "Weak block", "Weak → strong upgrade" and "weak refs requested while dying" sections:
  - **Last release:** CAS `hdr->weak` from NULL to `DYING`. If that fails and the value is a block, set `dead` on it and spin with `os_yield`-style waiting until `pins == 0`. Then call `hdr->destroy(obj)`.
  - **`obs_mem_finalize`:** the same dead-marking with a pin wait, then CAS `hdr->weak` to `DYING` so later `get_weak` calls hand out a dead block. If a real block was attached, drop the object's own weak ref on it.
  - **Lazy creation:** a new block gets `weak_refs = 1`, the object's own ref. `obs_mem_get_weak` then adds the caller's ref.
  - Add `obs-mem.c` and `obs-mem.h` to `libobs/CMakeLists.txt`, and `obs-mem.h` to the public header list.
- [ ] **Step 5: Run the test.** Run `ctest --preset windows-x64 -R test-obs-mem`, or run the built `test-obs-mem.exe` directly. Expected: exit code 0.
- [ ] **Step 6: Write `reference-libobs-obs-mem.rst`.** One `.. function::` entry per API function, stating its NULL behaviour and that the last release may happen on any thread. Link it from the toctree.
- [ ] **Step 7: Commit:** `libobs: Add unified obs_mem reference counting`.

### Task 2: `obs_data`, `obs_data_array`, `obs_data_item`

**Files:** Modify `libobs/obs-data.c`. Test: `test/test-obs-mem/test-obs-mem.c`.

**Interfaces:** Consumes Task 1's API. Produces no new symbols.

- [ ] **Step 1: Add the failing tests:**
  - `test_data_item_release_quirk`: `obs_data_create`, then `obs_data_set_int(d, "a", 1)`, then `item = obs_data_item_byname(d, "a")`. After `byname` the item has two refs (the data's and ours), so `obs_data_item_release(&item)` must leave `item != NULL`. Take a second ref with `obs_data_item_byname` again, call `obs_data_item_remove(&item)` (which detaches the item and drops the data's ref), then call `obs_data_item_release(&item)` on the remaining ref: it must set `item == NULL`.
  - `test_data_item_next`: a data object with 3 keys; iterating with `obs_data_first` and `obs_data_item_next` visits 3 items and ends with `item == NULL`.
  - `test_data_array_refs`: `obs_data_array_create`, `addref`, then `release` twice; on a debug heap, no leak and no double free.
- [ ] **Step 2: Build and run** `test-obs-mem`. Expected: the new tests pass before the change; they are regression pins. Record that and continue.
- [ ] **Step 3: Convert the three structs.** Replace the `volatile long ref` member with `struct obs_mem_header hdr` as the **first** member. Each create calls `obs_mem_init(obj, destroy_fn)`, where `destroy_fn` is the existing destroy (adapted to `void *`). Every `os_atomic_inc_long(&x->ref)` becomes `obs_mem_acquire(x)`. `obs_data_addref`/`_release` and `obs_data_array_addref`/`_release` become `obs_mem_acquire`/`obs_mem_release`. `obs_data_item_release(**)` becomes `if (item && *item && obs_mem_release(*item)) *item = NULL;`. Each destroy calls `obs_mem_finalize(obj)` right before its `bfree`. `obs_data_item`'s `brealloc` at about line 332 stays as it is.
- [ ] **Step 4: Run** `test-obs-mem`. Expected: all pass.
- [ ] **Step 5: Commit:** `libobs: Use obs_mem for obs_data refcounting`.

### Task 3: Scene items and missing files

**Files:** Modify `libobs/obs-scene.h` (struct at line 30), `libobs/obs-scene.c` (about lines 2390–2410 and every `->ref` use), and `libobs/obs-missing-files.c`. Test: `test-obs-mem.c`.

**Interfaces:** Consumes Task 1's API.

- [ ] **Step 1: Add the failing test** `test_missing_file_refs`. Create a file with `OBS_MISSING_FILE_SCRIPT`, add it to two `obs_missing_files_t` lists with `obs_missing_files_append`, then destroy both lists. The file is destroyed exactly once, which you can check through a callback-free debug-heap leak check, or by asserting refs reach 0 via `obs_mem_get_ref == NULL` before the second destroy.
- [ ] **Step 2: Build and run.** Expected: it passes before the change (regression pin).
- [ ] **Step 3: Convert `obs_scene_item` and `obs_missing_file`** the same way as Task 2. The header goes first in each struct; `item->ref = 1` / `file->ref = 1` become `obs_mem_init`; increments become `obs_mem_acquire`; releases become `obs_mem_release`. `obs_sceneitem_destroy` and `obs_missing_file_destroy` call `obs_mem_finalize` before `bfree`. `obs_missing_file_destroy` stays exported, and calling it directly counts as a forced destroy. `active_refs` is a different counter and stays as it is.
- [ ] **Step 4: Run** `test-obs-mem`. Expected: all pass.
- [ ] **Step 5: Commit:** `libobs: Use obs_mem for scene item and missing file refcounting`.

### Task 4: Context objects (sources, outputs, encoders, services, canvases, obs_object)

**Files:**
- Modify `libobs/obs-internal.h`: lines 619–715 (the `obs_weak_ref` and `obs_weak_object` structs and inline helpers), and the typed `obs_weak_*` structs at about lines 723, 781, 1173, 1340 and 1489.
- Modify `libobs/obs.c`: `obs_context_init_control` at about line 2691, `obs_context_data_free`, `obs_free_data` at about lines 1030–1060, and the `obs_object_*` / `obs_weak_object_*` functions at about lines 3395–3460.
- Modify `obs-source.c` (about lines 34, 258, 552, 801–960), `obs-output.c`, `obs-encoder.c`, `obs-service.c` and `obs-canvas.c` (their `get_weak` and release blocks).

**Interfaces:**
- Consumes Task 1's API.
- Produces: `struct obs_context_data` starts with `struct obs_mem_header hdr`. The `control` and `destroy` members are removed. `obs_context_init_control(context, object, destroy)` keeps its name and becomes `obs_mem_init(object, destroy)`, which is valid because the header is at offset 0 of `object`.
- Every public `obs_weak_*_t *` is a cast of `obs_mem_weak_t *`. The internal typed `struct obs_weak_*` definitions are deleted.

- [ ] **Step 1: Add the failing test** `test_source_weak_in_destroy_signal`:
  - Call `obs_startup("en-US", NULL, NULL)`, then `s = obs_scene_create_private("t")`.
  - Connect `"destroy"` on `obs_source_get_signal_handler(obs_scene_get_source(s))` to a handler that calls `w = obs_source_get_weak_source(src)`, asserts `obs_weak_source_expired(w)` and `obs_weak_source_get_source(w) == NULL`, then calls `obs_weak_source_release(w)`.
  - Call `obs_scene_release(s)`, wait with `obs_wait_for_destroy_queue()`, then `obs_shutdown()`.
  - Add `test_source_weak_basic`: get a weak ref; upgrading gives the same pointer; after release and `obs_wait_for_destroy_queue`, the weak ref is expired.
- [ ] **Step 2: Build and run.** Expected: `test_source_weak_basic` passes (regression pin). Today `test_source_weak_in_destroy_signal` also passes, because the block is allocated eagerly. Both pin behaviour that must survive the change.
- [ ] **Step 3: Convert `obs_context_data`.** Remove `obs_weak_ref`, `obs_weak_object` and the `obs_ref_*` / `obs_weak_ref_*` inline helpers, and replace their uses:

  | Old | New |
  |---|---|
  | `obs_ref_addref(&ctx.control->ref)` | `obs_mem_acquire(obj)` |
  | `if (obs_ref_release(&control->ref)) { destroy; weak_release(control); }` | `obs_mem_release(obj)` |
  | `get_weak(x)` + `addref` | `(obs_weak_x_t *)obs_mem_get_weak(x)` |
  | `obs_weak_*_get_*` | `obs_mem_weak_get_strong` |
  | `obs_weak_*_expired` | `obs_mem_weak_expired` |
  | `obs_weak_*_references_*` | `obs_mem_weak_references` |
  | `obs_*_get_ref` | `obs_mem_get_ref` |
  | `obs_object_release` | keeps its `!obs`-first check, then `obs_mem_release` |

  Keep each function's existing checks exactly, per Global Constraints.
- [ ] **Step 4: Place `obs_mem_finalize`.** Call `obs_mem_finalize(obj)` in each type's actual free, immediately before `obs_context_data_free` (whose final `memset` would otherwise wipe the header): for sources inside `obs_source_destroy_defer`; for outputs, encoders, services and canvases in their `*_destroy`. The creation-failure paths (`obs-source.c:552`, `obs-encoder.c:140`, `obs-output.c:259`) and `obs_free_data`'s forced destroys go through the same `*_destroy`, so they are covered with no extra code.
- [ ] **Step 5: Build everything** with `cmake --build --preset windows-x64`. Expected: no errors in libobs, frontend, plugins or obs-scripting. That confirms obs.hpp and SWIG are unaffected.
- [ ] **Step 6: Run** `test-obs-mem`. Expected: all pass.
- [ ] **Step 7: Commit:** `libobs: Use obs_mem for context object refcounting`.

### Task 5: Newly refcounted types and their callback guards

**Files:**
- `obs-internal.h`: `obs_view` at line 281, `obs_display` at line 293, `obs_encoder_group` at line 1350.
- `obs-display.c`: `obs_display_destroy` at line 101.
- `obs-video.c`: `render_displays` at line 94.
- `obs-view.c`.
- `obs-canvas.c`: the embedded `view` at `obs-internal.h:741`.
- `obs-audio-controls.c`: the fader and volmeter structs at lines 44 and 64, the dispatch at lines 187–206, and destroy at lines 565 and 752.
- `obs-properties.c`: struct at line 197, destroy at line 274, `group_data_free`, and the modified and button callback sites.
- `obs-missing-files.c`: `obs_missing_files_destroy` at line 44.
- `obs-encoder.c`: `obs_encoder_group_destroy` and the `destroy_on_stop` path at about line 884.

**Interfaces:** Consumes Task 1's API. Each type's current `*_destroy` body moves into a `static void <type>_free_cb(void *obj)`, which is the header's destroy and ends with `obs_mem_finalize(obj); bfree(obj);`. The exported `*_destroy(x)` becomes `obs_mem_release(x)`.

- [ ] **Step 1: Add the failing test** `test_fader_release_in_callback`:
  - After `obs_startup`, call `f = obs_fader_create(OBS_FADER_LOG)` and `obs_mem_acquire(f)`.
  - Add a callback that calls `obs_mem_release(f)` once.
  - Call `obs_fader_set_db(f, -6.0f)`, which fires `signal_volume_changed`, then `obs_fader_destroy(f)`.
  - Expected: no deadlock or crash, and the debug heap reports no leak.
  - Add `test_properties_group_outlives_parent`: create parent `p` and group child `g` with `obs_properties_add_group(p, "g", "G", OBS_GROUP_NORMAL, g)`, then `obs_mem_acquire(g)` and `obs_properties_destroy(p)`. Expected: `obs_properties_get_parent(g) == NULL` and `obs_properties_first(g)` is usable; finish with `obs_properties_destroy(g)`.
- [ ] **Step 2: Build and run.** Expected: both FAIL. `obs_mem_acquire` on these types reads a header that doesn't exist yet, so they crash or misbehave.
- [ ] **Step 3: Convert display, view, fader, volmeter, properties, missing_files and encoder_group.** Each gets the header as its first member, `obs_mem_init` in its create, and `*_free_cb` as described in the Interfaces block. The canvas's embedded `view` gets `obs_mem_init(&canvas->view, NULL)` and is never released or finalised; the canvas keeps tearing it down with its existing internal call.
- [ ] **Step 4: Add the guards.**
  - **Display:** in `render_displays`, call `obs_mem_acquire(display)`, render, read `next`, then `obs_mem_release(display)`, all under the existing recursive `displays_mutex`.
  - **Fader and volmeter:** in `signal_volume_changed` and `signal_levels_updated`, wrap the locked loop in `obs_mem_acquire`/`obs_mem_release`, and release **after** `pthread_mutex_unlock`.
  - **Properties:** wrap each user-callback invocation (the modified callbacks, the button clicked callback, and the group modified path) in acquire/release on `get_topmost_parent(props)`.
  - **Groups:** `group_data_free` sets the child's `parent = NULL` before `obs_properties_destroy(child)`.
  - **Encoder group:** `destroy_on_stop` keeps deferring. `obs_mem_finalize` and `bfree` run wherever the final free actually happens today.
- [ ] **Step 5: Run** `test-obs-mem`. Expected: all pass.
- [ ] **Step 6: Commit:** `libobs: Refcount displays, views, faders, volmeters, properties and groups`.

### Task 6: Whole-build verification

**Files:** none, unless a fix is needed.

- [ ] **Step 1: Do a clean build** with `cmake --build --preset windows-x64 --config RelWithDebInfo`. Expected: success with no new warnings in `libobs/`.
- [ ] **Step 2: Run the automated tests.** Run `ctest --preset windows-x64`, or `test-obs-mem` directly, then the `test-input` targets. Expected: all pass.
- [ ] **Step 3: Do a manual smoke test** of the built OBS:
  - Add and remove a scene with a filter, a group and a media source.
  - Open and close a projector, which creates and destroys an `obs_display`.
  - Open source properties containing a group property and close them.
  - Start and stop a recording.
  - Load a Lua or Python script that releases sources.
  - Exit.
  - Expected: no crash. The log has no "Double destroy" and no "Tried to release" warnings, and the shutdown leak count (`Number of memory leaks`) equals the baseline from `sanity/33.0` built the same way.
- [ ] **Step 4: Report the results to the user.** No push unless they ask.
