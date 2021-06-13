#pragma once

#include <memory>
#include <chrono>
#include <thread>
#include <atomic>


/*
	life_lock facilitates weak and shared pointers to objects which extend those
		objects' life by delaying their destruction rather than deleting them.
		Consequently, life-locked objects don't need to be allocated with new or
		make_shared: they can exist on the heap or as members of other objects.
		By extension, this allows for control over which thread destroys the object.

	This is achieved using one of two classes.

	* life_locked wraps around the object for maximum safety.
	* life_lock can be a member of the object but must be used carefully to avoid data races.
	
	The other class defined here, shared_ref, is used to implement life_lock.
	
	This header was designed for use with asynchronous I/O delegates, allowing observers
		to be destroyed at will while protecting against concurrent access by callbacks.
		The one-time lock mechanism used here is much more lightweight than a mutex.
		In a typical application, the lock will only rarely create a delay.
*/


/*
	When defined to 1, some hacks are employed to shave the size of these classes down
		to a single pointer (typically saving 4-8 bytes wherever they are used).
*/
#ifndef LIFE_LOCK_COMPRESS
	#define LIFE_LOCK_COMPRESS 0
#endif

// Whether to use C++20 wait/notify behavior rather than our own spinlock.
#ifndef LIFE_LOCK_CPP20
	#if __cplusplus >= 202002L || _MSVC_LANG >= 202002L
		#define LIFE_LOCK_CPP20 1
	#else
		#define LIFE_LOCK_CPP20 0
	#endif
#endif

// If C++20 wait/notify is not used, life_lock spins for a few microseconds before performing exponential backoff.
#ifndef LIFE_LOCK_SPIN_USEC
	#define LIFE_LOCK_SPIN_USEC 4
#endif
#ifndef LIFE_LOCK_SLEEP_MAX_USEC
	#define LIFE_LOCK_SLEEP_MAX_USEC 100000
#endif


namespace edb
{
	/*
		Utility function for adaptive spinlock behavior.
	*/
	namespace detail
	{
		void life_lock_wait(std::atomic_flag &lock)
		{
#if LIFE_LOCK_CPP20
			// Use standard C++ library's notify mechanism.
			lock.wait(true, std::memory_order_acquire);
#else
			// 1: See if the lock is initially clear.
			if (!lock.test_and_set(std::memory_order_acquire)) return;
			
			// 2: Spin for a short time.
			auto start = std::chrono::steady_clock::now();
			do    {if (!lock.test_and_set(std::memory_order_acquire)) return;}
			while (std::chrono::steady_clock::now() - start < std::chrono::microseconds(LIFE_LOCK_SPIN_USEC));

			// 3: Wait increasing periods of time.
			size_t wait_usec = 1;
			while (lock.test_and_set(std::memory_order_acquire))
			{
				std::this_thread::sleep_for(std::chrono::microseconds(wait_usec));
				wait_usec *= 2;
				if (wait_usec > LIFE_LOCK_SLEEP_MAX_USEC) wait_usec = LIFE_LOCK_SLEEP_MAX_USEC;
			}
#endif
		}
	}


	/*
		shared_ref is a shared_ptr without the pointer.
			It holds ownership but doesn't provide access to the object.
			Consequently, on many platforms it can be smaller ("compressed").
	*/
	class shared_ref
	{
	public:
		// Create an empty shared_ref.
		shared_ref()                                            noexcept          {}
		
		// Create a shared_ref from a shared_ptr.
		template<typename T> shared_ref(std::shared_ptr<T> &&t) noexcept          {_init(std::move(t));}

		// Release the held reference.
		~shared_ref() noexcept                                                    {_clear();}
		void reset()  noexcept                                                    {_clear();}

		// Check if a reference is held.
		explicit operator bool() const noexcept                                   {return bool(_cb);}

		// Produce pointers using the same management
		template<class T> std::shared_ptr<T> get_shared(T *obj) const noexcept    {buf sp; return _view<T>(obj, sp);}
		template<class T> std::shared_ptr<T> get_weak  (T *obj) const noexcept    {buf sp; return _view<T>(obj, sp);}

		// Move/copy
		shared_ref                 (shared_ref&&o) noexcept    {_cb = std::move(o._cb); o._cb = 0;}
		shared_ref& operator=      (shared_ref&&o) noexcept    {_cb = std::move(o._cb); o._cb = 0; return *this;}
		shared_ref           (const shared_ref &o) noexcept    {          _init(o.get_shared<shared_ref>(this));}
		shared_ref& operator=(const shared_ref &o) noexcept    {_clear(); _init(o.get_shared<shared_ref>(this)); return *this;}


	private:
		template<class T> using sptr_ = std::shared_ptr<T>;
		struct buf     {void *v[sizeof(sptr_<void>)/sizeof(void*)];};
#if LIFE_LOCK_COMPRESS // Memory saving hack for typical stdlib implementations
		static_assert(sizeof(sptr_<void>) == sizeof(void*[2]), "shared_ptr<T> not compressible on this platform");
		inline static bool CBP()
		{
			// Optimizing compilers will typically turn this into a constant value 1.  Has some overhead in debug mode.
			buf b = {{(void*)0x1111,(void*)0x2222}};
			return reinterpret_cast<sptr_<void>*>(b.v)->get() != b.v[1];
		}
		uintptr_t _cb = 0;
		template<class T> void            _init(sptr_<T> &&t)          noexcept    {buf sp; new (sp.v) sptr_<T>(std::move(t)); _cb = uintptr_t(sp.v[CBP()]);}
		template<class T> const sptr_<T> &_view(T *obj, buf &sp) const noexcept    {sp.v[!CBP()] = obj; sp.v[CBP()] = (void*) (_cb); return *(sptr_<T>*) sp.v;}
		void                              _clear()                     noexcept    {buf sp; _view(this, sp).~shared_ptr(); _cb = 0;}
#else   // Standard implementation -- stores a full shared_ptr, the pointer part of which is unused.
		sptr_<void> _cb;
		template<class T> void     _init(sptr_<T> &&t)          noexcept    {_cb = std::move(t);}
		template<class T> sptr_<T> _view(T *obj, buf &sp) const noexcept    {return sptr_<T>(_cb, obj);}
		void                       _clear()                     noexcept    {_cb.reset();}
#endif
	};


	/*
		life_lock provides "special" weak and shared pointers to an object, which
			may exist anywhere including on the stack or as a member variable.
			The life_lock's destruction will be blocked until all shared pointers
			created from it cease to exist.
		
		After the life_lock is destroyed, all pointers derived from it will be
			expired, even if the protected object continued to exist.

		life_lock may be a member of the object to be protected, but in this case
			it should be destroyed early in the object's destructor in order to
			prevent data races with other threads.
			In particular, if life_lock is a member of an abstract class, child classes
			should call .destroy() in their destructors to avoid pure virtual calls.

		The below class life_locked wraps around objects and is easier to use safely.
			Consider using it instead if you aren't familiar with concurrency programming!
	*/
	class life_lock
	{
	public:
		// Construct an uninitialized life_lock.
		life_lock() noexcept                                                      {}

		// Initialize a life_lock.
		template<class T>              life_lock(T *ptr)              noexcept    : _ref(std::shared_ptr<std::atomic_flag>(&_lock(), unblocking_deleter{})) {}
		
		// Initialize a life_lock with an allocator.
		template<class T, class Alloc> life_lock(T *ptr, Alloc alloc) noexcept    : _ref(std::shared_ptr<std::atomic_flag>(&_lock(), unblocking_deleter{}, std::forward<Alloc>(alloc))) {}

		// Check if the life_lock is initialized.
		explicit operator bool() const noexcept                                   {return bool(_ref);}

		/*
			Get a weak_ptr to the object.
				shared_ptr created from this weak_ptr will block life_lock's destruction.
		*/
		template<class T> std::weak_ptr  <T> get_weak  (T *ptr) const noexcept    {return _ref.get_weak  (ptr);}
		
		// It's perfectly possible for this class to produce shared_ptr directly, but it's an awful idea!
		//template<class T> std::shared_ptr<T> get_shared(T *ptr) const noexcept    {return _ref.get_shared(ptr);}

		/*
			Destroy the reference, waiting for any shared_ptr to expire.
				After destruction, the life_lock will resume an ininitialized state.
		*/
		~life_lock() noexcept
		{
			destroy();
		}
		void destroy() noexcept
		{
			if (_ref)
			{
				// Place ownership of the object in a temporary shared pointer.
				std::shared_ptr<life_lock> temp_shared_ptr(_ref.get_shared<life_lock>(this));
				_ref.reset();

				// Repurpose this object as a lock, lock it and release the shared_ptr.
				_lock().test_and_set(std::memory_order_release);
				temp_shared_ptr.reset();
				
				// Wait for the lock to be released by the last shared_ptr.
				detail::life_lock_wait(_lock());
				
				// Clear the shared_ref, whose memory we reused for an atomic_flag.
				new (&_ref) shared_ref();
			}
		}


	protected:
		struct unblocking_deleter
		{
			void operator()(std::atomic_flag *lock) const noexcept
			{
				lock->clear();
#if LIFE_LOCK_CPP20
				lock->notify_one();
#endif
			}
		};
		shared_ref _ref = shared_ref();
		std::atomic_flag &_lock() noexcept    {return reinterpret_cast<std::atomic_flag&>(_ref);}
	};



	/*
		This class contains an object protected by a life_lock.
			weak and shared pointers to the object may be created.
			The object's destruction will be blocked until no shared pointers to it exist.
			Additionally, life_locked supports an "empty" state like std::optional.
	*/
	template<typename T>
	class life_locked
	{
	public:
		// Construct an empty weak_holder.
		life_locked()     : _lock() {}

		// Construct a weak_holder with T's constructor arguments, or T() for the default constructor.
		template<typename Arg1, typename... Args>
		life_locked(Arg1 &&arg1, Args&&... args)    {_lock = life_lock(new (_t()) T (std::forward<Arg1>(arg1), std::forward<Args>(args)...));}

		// Wait until all shared_ptr have expired and destroy the contained object.
		~life_locked()    {destroy();}
		void destroy()    {if (_lock) {_lock.destroy(); raw_ptr()->~T();}}
		void reset()      {if (_lock) {_lock.destroy(); raw_ptr()->~T();}}  // "reset" alias for consistency with std::optional

		// Get weak pointer
		std::weak_ptr<T>       get_weak() const noexcept    {return _lock.get_weak(raw_ptr());}
		std::weak_ptr<const T> get_weak()       noexcept    {return _lock.get_weak(raw_ptr());}
		operator std::weak_ptr<T>      () const noexcept    {return _lock.get_weak(raw_ptr());}
		operator std::weak_ptr<const T>()       noexcept    {return _lock.get_weak(raw_ptr());}

		// Check on contained value
		bool has_value()         const noexcept    {return _lock;}
		explicit operator bool() const noexcept    {return _lock;}
		T       &value()       noexcept            {return *raw_ptr();}
		const T &value() const noexcept            {return *raw_ptr();}

		// Access the contained object.
		T       *raw_ptr   ()       noexcept    {return _lock ? _t() : nullptr;}
		const T *raw_ptr   () const noexcept    {return _lock ? _t() : nullptr;}
		T       *operator->()       noexcept    {return raw_ptr();}
		const T *operator->() const noexcept    {return raw_ptr();}
		T&       operator* ()       noexcept    {return *raw_ptr();}
		const T& operator* () const noexcept    {return *raw_ptr();}


		/*
			TODO: conform more closely to std::optional...
				- emplace
				- value_or
				- swap
				- std::hash ??
				- comparators
		*/  


	private:
		alignas(T) char _obj[sizeof(T)];
		life_lock       _lock;
		const T        *_t() const noexcept     {return reinterpret_cast<const T*>(_obj);}
		T              *_t()       noexcept     {return reinterpret_cast<      T*>(_obj);}
	};
}
