# Unified reference counting in libobs

Branch: `33.0/libobs-refcount`, based on `sanity/33.0` (65c320b46).

## Intent

libobs counts references in at least five different ways today:

- `obs_context_data` objects (sources, outputs, encoders, services, canvases) use a separately allocated control block (`struct obs_weak_ref` inside the typed `obs_weak_*` structs). Counts start at 0 and the object dies at -1.
- `obs_data`, `obs_data_array`, `obs_data_item`, `obs_sceneitem` and `obs_missing_file` each use their own `volatile long ref`, starting at 1 and dying at 0.
- `obs_display`, `obs_view`, `obs_fader`, `obs_volmeter`, `obs_properties`, `obs_missing_files` and `obs_encoder_group` aren't refcounted at all; they use create/destroy.

The goal is to replace all of these with one mechanism: one header struct, one counting API and one weak-reference scheme. Every existing per-type function keeps its name, signature and behaviour.

## Goals

- One header, `struct obs_mem_header`, embedded as the **first member** of every refcounted CPU-side `obs_*` object.
- One API: `obs_mem_acquire`, `obs_mem_release`, `obs_mem_get_weak`, `obs_mem_weak_acquire`, `obs_mem_weak_release` and `obs_mem_weak_get_strong`.
- Every existing `*_addref`, `*_get_ref`, `*_release`, `obs_weak_*_*` and `*_get_weak_*` becomes a thin **exported function** whose body calls the unified API.
- Newly refcounted types: their existing `*_destroy` becomes "release the creator's reference".

## Non-goals

- GPU objects (`gs_*`). A refcounted GPU API that survives device resets is a separate, later project tied to other graphics work. This design only has to leave the header usable by it: nothing in the header is specific to sources or contexts, and the API is exported from libobs.
- `os_*` utilities, media-io (`audio_t`, `video_t`), `signal_handler_t` and `proc_handler_t`.
- `obs_property_t`: it lives inside its `obs_properties` and has no lifetime of its own.
- `obs_hotkey*`: the API works by id, and the structs are stored inline in reallocating arrays.
- `obs_module_t`: loaded once and freed at shutdown; libobs doesn't support unloading.
- New per-type addref functions for the newly refcounted types. `obs_mem_acquire` covers them.

## Anti-goals (a failure even if it works)

- Any change to exported symbols, signatures or observable behaviour for plugins.
- Breaking obs.hpp. Its wrappers take release functions as template function pointers, so the aliases must stay real functions, never macros.
- Breaking the SWIG bindings (Lua and Python). They `%include` `obs.h`, `obs-source.h`, `obs-data.h` and `obs-properties.h`, so as long as those declarations don't change, the bindings don't either.
- Changing **when** or **on which thread** objects are destroyed while only their creator holds them, which is every existing caller. In particular, sources must still be destroyed on the destroy thread. Once a plugin takes its own ref through the public API, the last release decides the timing; see the guards below.

## Constraints

- `bmalloc` aligns to 32 bytes (`ALIGNMENT 32` in `libobs/util/bmem.c`), but `brealloc` may only keep 16 on non-Windows platforms. The header is exactly 32 bytes, so whatever follows it keeps the allocation's alignment.
- `long` is 32-bit on Windows; counts stay `volatile long`, as today.
- `libobs/util/threading-*.h` has no pointer atomics. The design needs pointer compare-exchange and load, so `os_atomic_*_ptr` helpers get added for both Windows and POSIX.
- `obs_data_item` reallocates itself (`obs-data.c:332`). That's fine with an embedded header, but such objects can never have a weak block, because the weak block points back at the object.
- `obs_context_data` is the first member of sources, outputs, encoders, services and canvases, and code casts freely between them and `obs_object_t*`. Making the header the first member of `obs_context_data` keeps the header at offset 0 for all of them.

## Approaches considered

1. **Header in front of the allocation.** `obs_mem_alloc(size, dtor)` returns `ptr + 32` and the header sits at `ptr - 32`. Struct layouts stay untouched, but every `bfree`/`brealloc` of these objects must become `obs_mem_free`/`obs_mem_realloc`, including the deferred source free, `obs_data_item`'s realloc and every `*_destroy`. A missed one corrupts the heap at runtime with no compile-time signal. Backend DLLs would also have to allocate through libobs.
2. **Header as the first struct member (chosen).** The memory layout is the same as option 1, but allocation stays `bzalloc`/`bfree`/`brealloc`. Struct layouts change, but every affected type is opaque to plugins.
3. **A separate control block holding both counts**, which is today's scheme applied everywhere. It needs a second allocation for every object, including `obs_data`, and an extra pointer hop on every acquire. Rejected in favour of keeping the strong count inside the object.

Chosen by the user: **2**.

## Design (decided)

### Header

```c
struct obs_mem_header {          /* 32 bytes, first member of every refcounted object */
	volatile long refs;      /* strong count; starts at 1, object destroyed at 0 */
	/* padding to 8 */
	struct obs_mem_weak *volatile weak; /* NULL until the first weak ref is requested */
	void (*destroy)(void *obj); /* called once when refs hits 0 */
	volatile long flags;     /* spare; e.g. "destroying" for double-destroy detection */
	/* padding to 32 */
};
```

Use `static_assert(sizeof(struct obs_mem_header) == 32)`. The size is fixed with explicit padding (a union with `char reserved[32]`), not an alignment attribute: with `ALIGNMENT_HACK`, `brealloc` can return 16-aligned blocks (`bmem.c`), and `obs_data_item` is `brealloc`'d, so declaring 32-byte alignment would be undefined behaviour. 32 is a multiple of 16, so members after the header keep whatever alignment the allocation has (32 for `bmalloc`).

### Counting rules

- Every count starts at 1 when the object is created and the object is destroyed when it reaches 0 (user decision). The context objects' 0/-1 convention goes away; callers can't see that.
- `obs_mem_acquire(obj)` is an unconditional atomic increment. It is used only when the caller already holds a strong reference (the `*_addref` semantics).
- `obs_mem_get_ref(obj)` (the `*_get_ref` semantics) increments only while the count is above 0, using a compare-and-swap loop like today's `obs_weak_ref_get_ref`, and returns NULL if the object is already dying.
- `obs_mem_release(obj)` is an atomic decrement. Only the release that reaches 0 does anything further (see below).

### Weak block

```c
struct obs_mem_weak {
	volatile long weak_refs; /* starts at 1: the object's own reference */
	volatile long pins;      /* upgrades in progress */
	volatile long dead;      /* set once refs hit 0 */
	void *object;            /* never cleared, so the references_* identity checks keep working */
};
```

- **Created lazily.** `obs_mem_get_weak(obj)` reads `hdr->weak`. If it's NULL, it allocates a block and compare-and-swaps it in; the loser frees its copy. It then increments `weak_refs` and returns the block. Objects that never hand out weak refs cost nothing extra.
- **Weak refs requested while the object is dying** (my call, from the builder check). Signal handlers on `"destroy"` and `info.destroy` run after the last release and can call `obs_*_get_weak_*`. Today they get a block that reports expired, and that behaviour is kept:
  - The last release swaps `hdr->weak` from NULL to a sentinel `OBS_MEM_WEAK_DYING` with a compare-and-swap. If a block already exists, it sets `dead` on it instead.
  - When `obs_mem_get_weak` sees the sentinel, it returns a fresh, unattached block that is already dead (`dead = 1`, `weak_refs = 1` owned by the caller). It never upgrades and is freed when the caller releases it.
  - There's no race with lazy creation: either `get_weak` installs its block before the release's compare-and-swap, in which case the release finds the block and marks it dead, or it sees the sentinel.
- **The weak block is freed when `weak_refs` reaches 0.** The object holds one weak reference to its own block. That reference is dropped by `obs_mem_finalize(obj)`, which every type's free path calls **immediately before its final `bfree`**: for sources at the end of `obs_source_destroy_defer`, and for context types before `obs_context_data_free`'s `memset`. So `hdr->weak` stays valid for the whole of a deferred destroy. A missed `obs_mem_finalize` leaks a block; it never corrupts memory.
- **Forced destroys** (my call). `obs_free_data` destroys leaked objects whose `refs` is still above 0, and the creation failure paths call `*_destroy` directly. `obs_mem_finalize` therefore also sets `dead` and waits for `pins == 0`, the same as the last release. That way an outstanding weak ref to a force-destroyed object expires instead of upgrading into freed memory. Today's code has the same hole: the upgrade succeeds and returns a dangling pointer. An upgrade that wins between the start of a forced destroy and `obs_mem_finalize` is not prevented, which matches today's behaviour; it only happens at shutdown, and creation failures have handed out no weak refs.
- **Embedded objects.** `struct obs_view` is embedded inline in `struct obs_canvas` (`obs-internal.h:741`). The embedded copy's header is initialised with `refs = 1` and is never released or finalised; the canvas tears it down with its existing internal free. `obs_view_destroy` is never called on it, which is already true today since it would `bfree` the middle of a canvas.
- `obs_encoder_group_destroy` can defer its free to the encoder stop path (`destroy_on_stop`, `obs-encoder.c:884`). It works unchanged as `hdr->destroy`, and `obs_mem_finalize` runs wherever the final `bfree` actually happens.
- The public typed weak pointers (`obs_weak_source_t*`, `obs_weak_object_t*` and so on) all point at the same `struct obs_mem_weak`. The typed structs disappear internally; the public typedefs stay opaque and unchanged.

### Weak → strong upgrade (the pin protocol)

Each operation is atomic on its own, but an upgrade needs two of them on two different pieces of memory: a read of the weak block, then a compare-and-swap of `refs` inside the object. Without the pin counter, the object can be freed between those two steps, and the compare-and-swap then writes into freed memory.

```
upgrade(w):                              last release (refs just hit 0):
  inc(w->pins)                             if (hdr->weak) {
  if (!load(w->dead))                          store(w->dead, 1)
      ok = CAS hdr(w->object)->refs            while (load(w->pins)) spin/yield
           from n>0 to n+1                   }
  dec(w->pins)                             hdr->destroy(obj)
  return ok ? w->object : NULL             obs_mem_weak_release(w)  /* object's own ref */
```

- Only the release that takes `refs` to 0 waits, and only if a weak block exists and an upgrade is in progress at that moment. An upgrade that starts after `dead` is set backs off at once, and any compare-and-swap after `refs` reached 0 fails. That limits the wait to upgrades already in flight, each a few instructions long.
- `dead` is used instead of clearing `object`. That keeps `obs_weak_*_references_*` working on expired weak refs, matching today's behaviour where `weak->source` is never cleared.
- `obs_mem_weak_expired(w)` returns `load(w->dead)`. Today's version checks the strong count. Both give the same answer once the object is dying.

### Destruction

- `hdr->destroy` is the type's existing destroy function. It still frees the object's memory itself, using `bfree` as today.
- Sources: `destroy` is `obs_source_destroy`. That function still unlinks the source inline and queues `obs_source_destroy_defer` on `obs->destruction_task_thread`. Because the object's own weak reference is dropped right after `destroy` returns, i.e. before the deferred task runs, the weak block can be freed before the deferred free. That's safe because the deferred task never touches the weak block. The source's memory stays alive until the deferred task `bfree`s it, as today.
- The source's existing `destroying` flag and "Double destroy" log stay as they are.
- Each typed function keeps **exactly** the checks it has today; none are added or removed. Today only `obs_source_release`, `obs_canvas_release` and `obs_object_release` check `!obs`, and `obs_object_release` checks it before the NULL check. None of these checks move into `obs_mem_release`.

### Existing API mapping (all remain exported functions)

| Existing | Becomes |
|---|---|
| `obs_{source,output,encoder,service,canvas,object}_get_ref` | `obs_mem_get_ref` |
| `obs_source_addref` (internal only: declared in `obs-internal.h`, not exported) | `obs_mem_acquire` |
| `obs_{source,scene,output,encoder,service,canvas,object}_release` | existing checks unchanged, then `obs_mem_release` |
| `obs_{source,output,encoder,service,canvas,object}_get_weak_*` | `obs_mem_get_weak` |
| `obs_weak_*_addref` / `_release` | `obs_mem_weak_acquire` / `obs_mem_weak_release` |
| `obs_weak_*_get_*` | `obs_mem_weak_get_strong` |
| `obs_weak_*_expired` | `obs_mem_weak_expired` |
| `obs_weak_*_references_*` | compare `w->object` |
| `obs_sceneitem_addref` / `_release` | `obs_mem_acquire` / `obs_mem_release` |
| `obs_data_addref` / `_release`, `obs_data_array_addref` / `_release` | `obs_mem_acquire` / `obs_mem_release` |
| `obs_data_item_release(**)` | `obs_mem_release`; sets `*item = NULL` only when that call destroyed the item (the existing quirk is kept) |
| `obs_missing_file_addref` / `_release` | `obs_mem_acquire` / `obs_mem_release` |
| `obs_{display,view,fader,volmeter,properties,missing_files,encoder_group}_destroy` | `obs_mem_release` (the creator's reference) |

Internal raw increments, such as `os_atomic_inc_long(&item->ref)` in `obs-data.c` and `obs-scene.c`, become `obs_mem_acquire`. `obs_context_data.control` and `obs_context_data.destroy` go away; their roles move into the header, and `obs_context_init_control` fills in the header.

### Where the API lives

- New header `libobs/obs-mem.h`, plus an implementation in `libobs/obs-mem.c`. **Public API** (user decision): the header is installed and documented, and plugins may call `obs_mem_*` on any refcounted `obs_*` object. The functions are `EXPORT`ed, so backend DLLs and the future `gs_*` work can use them too. These exports are additions, which don't break existing plugins.
- Not added to the SWIG `.i` files (my call). Scripts keep using the typed functions, and a `void *` API is of no use from Lua or Python.

### Guards needed because the API is public

Before this change, only the creator held the newly refcounted types, so `*_destroy` always ran when and where the creator chose. Once plugins can take refs, the last release can happen on any thread, including from inside a libobs callback for that same object. The destructors themselves are already safe to call from any thread: `obs_display_destroy` enters graphics, which is recursive per thread, and the others only lock mutexes. The hazard is **re-entrancy**: a destroy that runs while libobs is iterating over, or calling back through, the object. The rule is:

**Every libobs loop or callback dispatch that hands a newly refcounted object to user code holds a strong reference on that object for the duration of the call, and reads any "next" link before releasing that reference.**

Concretely:
- **Display.** `render_displays` (`obs-video.c:94`) holds the recursive `displays_mutex` while calling draw callbacks. A callback that drops the last ref would today unlink and free the node the loop is standing on. The fix: take a ref on the current display, render it, read `next`, then release. The final destroy then happens under the recursive mutex, after the loop has moved on.
- **Fader and volmeter.** Their callback dispatch, from the audio and source signal paths, holds a ref on the fader or volmeter while invoking callbacks.
- **Creator-only timing (decided in review).** Those temporary refs must not move a creator-only destroy to another thread. `obs_display_destroy` releases while holding graphics and then `displays_mutex` (the render thread's order), so the render thread's temporary ref is always gone first. `obs_fader_destroy` and `obs_volmeter_destroy` detach from the source before releasing; detach waits on the signal mutex or `audio_cb_mutex`, inside which the dispatch drops its temporary ref, and it stops callbacks for every other holder too.
- **Properties.** The modified, button and group callbacks hold a ref on the top-level `obs_properties_t` while running.
- **Properties groups.** `group_data_free` clears the child's `parent` link before releasing it. A group's properties held by a plugin then survive their parent as a detached set, and `obs_properties_get_parent` returns NULL instead of a dangling pointer.
- `obs_view`, `obs_missing_files` and `obs_encoder_group` hand themselves to no user callbacks, so they need no guard.
- The already-refcounted types (sources, outputs, encoders, services, canvases, `obs_data`, scene items) were already public, so their behaviour doesn't change.
- `os_atomic_compare_exchange_ptr`, `os_atomic_load_ptr` and `os_atomic_set_ptr` go into `util/threading-windows.h` and `util/threading-posix.h`.

## Left to the builder

- Exact padding syntax for the header (decided in review: explicit padding, no alignment attribute).
- Whether `obs_mem_acquire` and `obs_mem_release` are also `static inline` internally for speed, alongside the exported versions.
- Spin strategy while waiting on pins (`os_yield` or a plain pause loop is fine).
- The order in which types are converted. Suggested order: `obs-mem.c` with tests first, then `obs_data`, scene items, missing files, the context types, and finally the newly refcounted types.
- Tests: a cmocka test in `test/cmocka/` for `obs-mem` alone, covering the counting rules, lazy weak-block creation, expiry, a threaded upgrade-vs-release stress test, and the `obs_data_item` `**` quirk. Everything else is checked by building OBS and running it, including `test/test-input`.
