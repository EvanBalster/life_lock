# life_lock ☥
### observing absolutely any C++ object with `weak_ptr`

life_lock produces `weak_ptr` and `shared_ptr` referring to arbitrary objects, and uses a one-time blocking mechanism to guarantee that any resulting `shared_ptr` has expired before destruction.  This allows for greater flexibility when using smart pointers in multi-threaded applications:

* The object does not need to be allocated by `new`, `make_shared` or any other dynamic method.
* The object can be destroyed by the thread which created it, regardless of accessing threads.

This header was designed for observer patterns and asynchronous I/O, allowing objects to receive calls from arbitrary threads and preventing their destruction while calls are in progress.  The one-time blocking mechanism used here is much more lightweight than a mutex, using 1–3 words of memory and atomic waiting behavior at destruction time.



This library is not affiliated with any identity theft protection services.  :)



## Requirements

* A C++11 compatible compiler and runtime supporting:
  * `std::shared_ptr` & `std::weak_ptr`.
  * `std::atomic` integer types.



## How to use it

Copy `life_lock.hpp` into your include directory.

**Option 1**:  For greater safety, wrap the protected object in `life_locked<T>`.  This type behaves similar to `std::optional`, additionally providing a `get_weak` method for generating smart pointers.  The contained object is always destroyed *after* the Life Lock.

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
* Optionally call `retire()` to hasten the extinction of shared pointers.
* Call `destroy()`, which completes when all shared pointers made from the lock are extinct.

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



## Example

Here we have a list that exists as a local variable in `main()` and a generator thread which fills up that list.  We use `life_locked` to ensure the list is not destroyed while the generator thread is accessing it.

Note that this code would deadlock if the generator thread did not regularly release its strong reference.

```cpp
#include <memory>
#include <chrono>
#include <thread>
#include <vector>
#include <life_lock.hpp>

using namespace std::chrono;

void main()
{
	struct IntList
	{
		// Another thread fills this up...
		std::vector<int> ints;

		// But life_lock guarantees that's over by destruction time.
		~IntList() {std::cout << "~IntList() size: " << ints.size() << std::endl;}
	};

	edb::life_locked<IntList> my_ints;
	std::weak_ptr<IntList> weak_from_lifelock = my_ints.weak();

	std::cout << "Begin generating IntList..." << std::endl;

	std::thread generator([weak_from_lifelock]()
	{
		for (auto start_time = steady_clock::now(), current_time = start_time;
			current_time - start_time < seconds(2);
			current_time = steady_clock::now())
		{
			std::shared_ptr<IntList> strong_ref = weak_from_lifelock.lock();
			if (!strong_ref)
			{
				std::cout << "IntList generator interrupted!\n" << std::flush;
				return;
			}
			strong_ref->ints.push_back(current_time.time_since_epoch().count());
		}
		std::cout << "IntList generator was NOT interrupted?!\n" << std::flush;
	});

	std::this_thread::sleep_for(seconds(1));

	std::cout << "Destroying IntList...\n" << std::flush;
	my_ints.reset();
	std::cout << "Awaiting completion of generator...\n" << std::flush;
	generator.join();
	std::cout << "Generator thread stopped.\n" << std::flush;
}
```



## Pitfalls

1. `life_lock` does not protect against data races other than destruction.
   * If members are accessed by multiple threads prior to destruction, or through shared references not derived from `life_lock`, other forms of safe concurrency such as atomics and mutexes will be necessary.
2. If `life_lock` is a member of an abstract class, `destroy()` should be explicitly called from the destructors of any non-abstract child classes in order to avoid pure virtual function calls.
   * Avoid this problem by wrapping the actual object in `life_locked<T>`.
   * It's safe to call `destroy()` multiple times (for example, in base and derived class destructors).
3. Destroying `life_lock` in a thread that holds a shared pointer derived from it causes **deadlock**.
   * This is comparable to holding both forms of lock on a `shared_mutex`.
   * In most cases, `life_lock`-derived shared pointers should be used only in other threads.
4. If `life_lock`-derived shared pointers with long, overlapping lifespans may cause **livelock**.
   * Don't hold `life_lock`-derived shared pointers longer than is necessary.
   * `retire()` can be called before `destroy()`, providing more time for reference extinction.
   * Multiple `life_lock` can safely protect the same object, if needed to reduce overlap.

Fundamentally, a `shared_ptr<T>` created by `life_lock` behaves differently than one created by `make_shared<T>`. despite being the same type.  The former *delays* destruction while the latter *controls* it.  It's possible and safe to use both forms of `shared_ptr` to refer to the same object.



## How it Works

`life_lock` internally has two working parts:

* An atomic state, which can be:
  * `working` — we can make smart pointers to a protected object
  * `retired` — no more smart pointers can be made, but some may still exist
  * `expired` — there are no smart pointers left, but `destroy()` has not completed
  * `empty` — no object is being protected
* A shared reference to the atomic state above.

Initializing the `life_lock` sets its atomic state to `working`.  An uninitialized lock is `empty`.

The methods `get_weak(p)` and `get_shared(p)` produce smart pointers *aliased* to `p`, whatever its type.

Calling `retire()` or `destroy()` on a working lock releases the shared reference and sets the atomic state to `retired`.  Afterwards, once all remaining shared references have expired, that state is updated to `expired` by a special deleter installed in the shared reference.

Calling `destroy()` waits until the lock's atomic state is `expired`, then updates it to `empty`.

##### Atomic Waiting

If `LIFE_LOCK_CPP20` is defined to `1` or undefined on a C++ compiler, `atomic_flag::wait`/`notify_one` will be used to wait until the lock is `expired`.  Otherwise, a configurable "timed backoff" behavior will be used:

* Most often, the `life_lock` itself held the only reference and no delay is needed.
* Otherwise, spin for up to `LIFE_LOCK_SPIN_COUNT` times.
* Then, sleep with exponential backoff from 1 up to `LIFE_LOCK_SLEEP_MAX_USEC` microseconds.



## Evil Hacks for Memory Efficiency

The library provides two optional macros and a supplementary header which can reduce the memory overhead of `life_lock` to the size of a single pointer by relying on assumptions about implementation of smart pointers.

Because these 'enhancements' exploit undefined behavior, they should not be used without testing to ensure their well-functioning on any given platform.  Unless an application is creating millions of "life-locked" objects, efficiency gains are probably negligible.

##### Default Implementation (neither flag is defined)

`life_lock`'s default implementation contains an atomic integer (the state) and a `shared_ptr` referring to that integer.  This implementation does not rely on undefined behavior and is C++11 compliant.

##### `LIFE_LOCK_COMPRESS`

This hack reduces the size of `life_lock` by approximately 1 pointer, by assuming that:

* `shared_ptr` is internally made up of control pointers and a referent pointer.
* the pointers inside `shared_ptr<atomic<uintptr_t>>`, if cast to integers, never equal `1` or `2`.

When enabled, the shared reference (which is ) and atomic state are placed together in a `union`.  In `empty` and `working` states, this object is treated as a shared reference; in `retired` and `expired` states it is treated purely as an atomic value.

##### `LIFE_LOCK_COMPRESS` and `SHARED_PTR_HACKS`

When both options are combined, `life_lock` utilizes the condensed implementation from `shared_anchor.hpp` in order to hold only a pointer to the control block of its shared reference.  No pointer to the atomic state is necessary because this pointer would point *to itself*.

This implementation relies on extensive assumptions about the platform's smart pointers:

* `shared_ptr` and `weak_ptr` are internally made up of a normal, untagged pointer to the referent and an additional pointer-sized field which refers to the control block, with no padding.
  * The ordering of these elements (A-B or B-A) is consistent between all shared and weak pointers.
* The control block pointer may be extracted and used to manually create new shared and weak pointers to arbitrary types.

These assumptions are known to hold on the following platforms:

* MSVC / Windows x64, Debug and Release modes
* Clang / Mac OS X x64, Debug and Release modes

With a typical optimized implementation of the standard C++ library...

| Structure                            | Default               | LIFE_LOCK_COMPRESS    | LIFE_LOCK_COMPRESS<br/>SHARED_PTR_HACKS |
| ------------------------------------ | --------------------- | --------------------- | --------------------------------------- |
| `shared_anchor`                      | 2 words (not used)    | 2 words               | 1 word                                  |
| `life_lock` or<br />`life_lock_self` | 3 words               | 2 words               | 1 word                                  |
| `life_locked<T>`                     | 3 words + `sizeof(T)` | 2 words + `sizeof(T)` | 1 word + `sizeof(T)`                    |

Use this uncouth magic at your own risk!