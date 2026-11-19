Reference Counting (obs_mem)
============================

A single reference counting and weak reference mechanism for objects in
libobs. An object embeds :c:struct:`obs_mem_header` as its **first member**
and is allocated with :c:func:`bzalloc`. Counts start at 1 and the object is
destroyed when its count reaches 0.

.. code:: cpp

   #include <obs-mem.h>

The last :c:func:`obs_mem_release` may happen on any thread, including from
inside a callback of the object itself.

These functions work on any refcounted ``obs_*`` object: sources, outputs,
encoders, services, canvases, scenes, scene items, :c:type:`obs_data_t`,
:c:type:`obs_data_array_t`, missing files, displays, views, faders,
volmeters, properties and encoder groups.

- For displays, views, faders, volmeters, properties, missing files and
  encoder groups, the type's ``*_destroy`` function releases the creator's
  reference. The object is only freed once every other reference taken with
  :c:func:`obs_mem_acquire` or :c:func:`obs_mem_get_ref` has been released
  too. :c:func:`obs_fader_destroy` and :c:func:`obs_volmeter_destroy` also
  detach the object from its source, for every holder: once they return, no
  further callbacks of that fader or volmeter run. :c:func:`obs_display_destroy`
  still frees the display on the calling thread when nobody else holds it.
- :c:func:`obs_mem_release` does not perform the checks of the typed release
  functions. For example :c:func:`obs_source_release` does nothing once libobs
  has shut down, while :c:func:`obs_mem_release` always releases.
- Do not use :c:func:`obs_mem_get_weak` on an :c:type:`obs_data_item_t`: an
  item reallocates itself when its value changes, which would leave the weak
  reference pointing at freed memory.


Structures
----------

.. struct:: obs_mem_header

   Header that must be the first member of every refcounted object. It is
   32 bytes in size, a multiple of 16, and has no alignment requirement
   beyond that of a pointer, so it also works in memory returned by
   :c:func:`brealloc`. The members are private.


Strong Reference Functions
--------------------------

.. function:: void obs_mem_init(void *obj, obs_mem_destroy_cb destroy)

   Initializes the header of *obj* with a reference count of 1.

   :param obj:     Object whose first member is a :c:struct:`obs_mem_header`
   :param destroy: Called once when the last reference is released. It must
                   call :c:func:`obs_mem_finalize` and then free the object
                   with :c:func:`bfree`.

   A NULL *obj* is ignored.

---------------------

.. function:: void *obs_mem_acquire(void *obj)

   Adds a reference without any check. The caller must already hold a
   reference.

   :return: *obj*. A NULL *obj* is ignored and NULL is returned.

---------------------

.. function:: void *obs_mem_get_ref(void *obj)

   Adds a reference only if the object is not already being destroyed.

   :return: *obj*, or NULL if the object is dying or *obj* is NULL

---------------------

.. function:: bool obs_mem_release(void *obj)

   Releases a reference. When it was the last one, the destroy callback runs
   on the calling thread.

   :return: true if this call destroyed the object. A NULL *obj* returns false.

---------------------

.. function:: void obs_mem_finalize(void *obj)

   Must be called immediately before the final :c:func:`bfree` of the object.
   Marks all weak references as expired, which also covers objects that are
   destroyed while references are still held, and drops the object's own
   reference to its weak block. A NULL *obj* is ignored.


Weak Reference Functions
------------------------

.. function:: obs_mem_weak_t *obs_mem_get_weak(void *obj)

   Gets a new weak reference to the object. Repeated calls return the same
   pointer with one more reference each. Release it with
   :c:func:`obs_mem_weak_release`. If the object is already being destroyed,
   the returned weak reference is expired, and each call may return a
   different pointer.

   Must not be used on an :c:type:`obs_data_item_t`.

   :return: A weak reference, or NULL if *obj* is NULL

---------------------

.. function:: void obs_mem_weak_acquire(obs_mem_weak_t *weak)

   Adds a reference to a weak reference. A NULL *weak* is ignored.

---------------------

.. function:: void obs_mem_weak_release(obs_mem_weak_t *weak)

   Releases a weak reference. A NULL *weak* is ignored.

---------------------

.. function:: void *obs_mem_weak_get_strong(obs_mem_weak_t *weak)

   Upgrades a weak reference to a new strong reference. Safe to call from
   any thread while the object is being destroyed.

   :return: The object with a new reference, or NULL if it has expired or
            *weak* is NULL

---------------------

.. function:: bool obs_mem_weak_expired(obs_mem_weak_t *weak)

   :return: true if the object was destroyed, or *weak* is NULL

---------------------

.. function:: bool obs_mem_weak_references(obs_mem_weak_t *weak, void *obj)

   Checks whether *weak* refers to *obj*. This still works after the object
   expired, and never dereferences *obj*.

   :return: true if *weak* was created for *obj*; false if *weak* is NULL
