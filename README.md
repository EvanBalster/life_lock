# life_lock ☥
Enhancing C++ weak_ptr for concurrent callbacks

life_lock facilitates weak and shared pointers to objects which extend those objects' life by delaying their destruction rather than deleting them.  Consequently, life-locked objects don't need to be allocated with `new` or `make_shared`: they can exist on the heap or as members of other objects.  By extension, this allows for control over which thread destroys the object.

This is achieved using one of two classes:

* `life_locked<T>` wraps around the object for maximum safety, behaving similar to `std::optional<T>`.
* `life_lock` can be a member of the object or stored separately.  It must be used carefully to avoid data races.

A third class, `shared_retainer` is a utility used to implement `life_lock` and `LIFE_LOCK_COMPRESSED`.

This header was designed for use with asynchronous I/O delegates, allowing observers to be destroyed at will while protecting against concurrent access by callbacks.  The one-time lock mechanism used here is much more lightweight than a mutex.  In a typical application, objects will only rarely need to delay their destruction.

## A word on Deadlock

Attempting to destroy a `life_lock` while the same thread holds a `shared_ptr` created from it will cause the thread to deadlock.  In this sense, holding a `shared_ptr` is much like holding a lock — and similar wisdom applies.

The most typical use of `life_lock` involves creating `weak_ptr` instances which are locked in order to signal events or messages to an object, and released shortly afterward.

## How it Works

`life_lock`, via its sole member `shared_retainer`, holds *part of* a shared_ptr to the object — specifically, the "control block" containing reference counts.  The retainer holds ownership over the object just like `shared_ptr` but does not provide a pointer to the object.

`life_lock` creates a `shared_retainer` with a special deleter.  When the retainer and all `shared_ptr` derived from it have expired, the deleter releases the `life_lock`.  The destructor of `life_lock` follows this sequence:

* Create a `shared_ptr` from the retainer.
* Destroy the retainer, repurposing its memory as an `atomic_flag`.
* Lock the `atomic_flag`.
* Destroy the `shared_ptr`.
* Wait for the `atomic_flag` to be unlocked...
  * Initially try spinning...
  * Then follow an exponential backoff from 1 microsecond up to .26 seconds.

## Compressed life_lock

When `LIFE_LOCK_COMPRESSED` is not defined (the default), `shared_retainer` is implemented with `shared_ptr<void>`, which is typically the size of two pointers.  If `LIFE_LOCK_COMPRESSED` is defined, the library will dissect `shared_ptr` in order to compress it down to a single pointer.

Compression is based on the assumption that a `shared_ptr` contains a pointer to the object and one additional pointer-sized member whose least significant bit is never set.
