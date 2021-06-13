# life_lock ☥
#### enhancing C++ `weak_ptr` for concurrent callbacks

life_lock creates weak and shared pointers that *delay* the destruction of an object rather than taking over its deletion.  Life-locked objects don't need to be allocated with `new` or `make_shared`: they can exist as local variables on the stack or as members of other classes.  By extension, this allows for control over which thread destroys the object.

This header was designed for use with observer patterns and asynchronous I/O, allowing callers to concurrently access objects they don't own.  The one-time lock mechanism used here is much more lightweight than a mutex, using just one or two words of memory and atomic waiting behavior at destruction time.

## Requirements

* A C++11-compatible compiler and runtime with basic support for `std::shared_ptr`, `std::weak_ptr` and atomics.

## How to use it

Copy `life_lock.h` into your include directory.

**Option 1**:  For greater safety, wrap the protected object in `life_locked<T>`.  This type behaves similar to `std::optional`, additionally providing a `get_weak` method for utilizing smart pointers.  The contained object is always destroyed *after* the Life Lock.

> `life_locked<T> myObject(T())`

**Option 2**:  For greater flexibility, instances of the `life_lock` class can exist inside (or outside!) the object to be protected.  In this case, Life Locks should usually be destroyed early in the object's destructor, before any concurrently-accessed members are torn down.

> `T myObject;  // has a life_lock member`

**Either way**...

* Obtain weak pointers via the `get_weak()` method.
* Distribute those pointers to the object's callers.
* When the object is needed (usually for a callback):
  * Call `lock()` on the weak pointer to get a temporary shared pointer.
    * If the shared pointer is null, the object has already disappeared!
    * Otherwise, the object is guaranteed to exist at least as long as the shared pointer.
  * Release the shared pointer as soon as you can to keep things running smoothly.

> ```C++
> auto weakPtr = myObject.get_weak();
> // ... often, weak pointer is passed to another thread ...
> {
>     auto myObjectPtr = weakPtr.lock();
> 	myObjectPtr->receiveMessage("hello");
>     // Shared pointer goes out of scope at the end of the block.
> }
> ```

It is possible to have an unlimited number of Life Locks protecting the same object (or nested objects).  This can mitigate contention if many different threads need to concurrently access the object.

## Pitfalls

1. If `life_lock` is a member of an abstract class, `destroy()` should be explicitly called from the destructors of any non-abstract child classes in order to avoid pure virtual function calls.
2. Destroying a `life_lock` in a thread that holds a shared pointer created from it will cause **deadlock**.  (Avoid this by holding weak pointers instead.)
3. If other threads hold shared pointers derived from a life_lock for long, overlapping periods of time, **livelock** may result.  (Avoid this by holding shared pointers as briefly as possible, and/or creating multiple Life Locks.)
4. `life_lock` does not protect against data races other than destruction.  If multiple threads interact with the object's members during its lifetime, other forms of synchronization will be necessary.
5. Remember that `life_lock` pointers behave differently than those created with `make_shared` or `new`, despite being the same type.  It's possible (and sometimes useful) to use both kinds of shared_ptr with the same object at the same time.

## How it Works

The class `shared_ref` holds *part of* a shared_ptr to the object — specifically, the "control block" containing reference counts.  The ref holds ownership over the object just like `shared_ptr` but does not provide a pointer to the object.

A newly-initialized `life_lock` creates and holds a `shared_ref` with a special "unblocking deleter".  When the ref and all `shared_ptr` derived from it have expired, the deleter permits `life_lock` to complete its destruction.  The destructor of `life_lock` follows this sequence:

* Create a temporary `shared_ptr` from the `shared_ref`.
* Destroy the `shared_ref`, repurposing its memory as a locked `atomic_flag`.
* Destroy the temporary  `shared_ptr`.
* Wait for the `atomic_flag` to be unlocked.

If `LIFE_LOCK_CPP20` is defined to `1` or undefined on a C++ compiler, `atomic_flag::wait`/`notify_one` will be used for waiting on the atomic flag.  Otherwise, a configurable "timed backoff" behavior will be used:

* Most often, the `life_lock` itself held the only reference and no delay is needed.
* Otherwise, spin for up to `LIFE_LOCK_SPIN_USEC` microseconds.
* Then, check and sleep with exponential backoff from 1 microsecond up to `LIFE_LOCK_SLEEP_MAX_USEC` microseconds.

## Efficiency and Optimization via Compression

With a typical optimized implementation of the standard C++ library...

| Structure        | Normal Size           | LIFE_LOCK_COMPRESS   |
| ---------------- | --------------------- | -------------------- |
| `shared_ref`     | 2 words               | 1 word               |
| `life_lock`      | 2 words               | 1 word               |
| `life_locked<T>` | 2 words + `sizeof(T)` | 1 word + `sizeof(T)` |
| control block    | 2 words               | 2 words              |

`life_lock` only *needs* one word of memory — a pointer to the control block.  By default, `shared_ref` is implemented with `shared_ptr<void>`, which additionally contains a pointer to the shared object, which `shared_ref` does not use.

Defining `LIFE_LOCK_COMPRESSED` to `1` will enable a compression hack.  While this hack is technically non-standard and non-portable, it should work as expected with most C++11 libraries.  Compression shrinks `shared_ref` to a single pointer, based on the assumption that a `shared_ptr` has the following contents:

* The pointer, which is either in position 0 or, if in position 1, is not tagged or otherwise modified.
* One additional pointer-sized member (which refers somehow to the control block).

`LIFE_LOCK_COMPRESSED` may only work in release mode on some platforms.