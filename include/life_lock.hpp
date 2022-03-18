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

	* life_locked wraps around the object.  (this is the safest & easiest way)
	* life_lock can be a member of the object.  (destroy() early to avoid data races!)
	
	This header was designed for use with multithreaded observers/receivers, allowing them
		to be destroyed at will while protecting against concurrent access by callbacks.
		The one-time lock mechanism used here is much more lightweight than a mutex.
		In a typical application, the lock will only rarely create a delay.
		The retire() function may be used to further reduce the chance of blocking.
*/


/*
	Variant implementation options.
	
	LIFE_LOCK_COMPRESS enables a sentinel value hack to reduce life_lock size by 1 word.

	SHARED_PTR_HACKS can reduce size of life_lock to 1 word on platforms where assumptions
		about the implementation of shared_ptr hold.
		When combined with LIFE_LOCK_COMPRESS this yields a size of 1 pointer.
*/
#ifndef LIFE_LOCK_COMPRESS
	#if SHARED_PTR_HACKS
		#define LIFE_LOCK_COMPRESS 1
	#else
		#define LIFE_LOCK_COMPRESS 0
	#endif
#endif

#if LIFE_LOCK_COMPRESS
	#ifndef SHARED_PTR_HACKS
		#define SHARED_PTR_HACKS 1
	#endif
#endif

#if LIFE_LOCK_COMPRESS
	#include <shared_anchor.hpp>
#endif

// Whether to use C++20 wait/notify behavior rather than our own spinlock.
#ifndef LIFE_LOCK_CPP20
	#if __cpp_lib_atomic_wait || __cplusplus >= 202002L || _MSVC_LANG >= 202002L
		#define LIFE_LOCK_CPP20 1
	#else
		#define LIFE_LOCK_CPP20 0
	#endif
#endif
#ifndef LIFE_LOCK_FALLTHROUGH
	#if __cplusplus >= 201700L || _MSVC_LANG >= 201700L
		#define LIFE_LOCK_FALLTHROUGH [[fallthrough]]
	#else
		#define LIFE_LOCK_FALLTHROUGH
	#endif
#endif

// If C++20 wait/notify is not used, life_lock spins a few times before performing exponential backoff.
#ifdef LIFE_LOCK_SPIN_USEC
	#error "LIFE_LOCK_SPIN_USEC is no longer used."
#endif
#ifndef LIFE_LOCK_SPIN_COUNT
	#define LIFE_LOCK_SPIN_COUNT 40
#endif
#ifndef LIFE_LOCK_SLEEP_MAX_USEC
	#define LIFE_LOCK_SLEEP_MAX_USEC 100000
#endif


namespace edb
{
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
		enum status_t : uintptr_t
		{
			working = 3, // don't modify these values.
			retired = 2,
			expired = 1,
			empty   = 0
		};

	public:
		// Construct an uninitialized life_lock.
		life_lock() noexcept                                             : life_lock(_internal_tag{}) {}
		~life_lock() noexcept                                            {destroy(); _destruct();}

		// Construct a life_lock in initialized state.
		template<class T>              life_lock(T *ptr)                 : life_lock(_internal_tag{}, std::shared_ptr<_status_word>(&_status, _deleter{})) {}
		
		// Initialize a life_lock with an allocator.
		template<class T, class Alloc> life_lock(T *ptr, Alloc alloc)    : life_lock(_internal_tag{}, std::shared_ptr<_status_word>(&_status, _deleter{}, std::forward<Alloc>(alloc))) {}


		// Manually initialize a previously uninitialized life_lock.
		void                       init()                                {if (status() == empty) _ref = std::shared_ptr<_status_word>(&_status, _deleter{});}
		template<class Alloc> void init(Alloc alloc)                     {if (status() == empty) _ref = std::shared_ptr<_status_word>(&_status, _deleter{}, std::forward<Alloc>(alloc));}


		/*
			Check the status of the life_lock.
		*/
		explicit operator bool() const noexcept                          {return status() == working;}
		bool is_working()        const noexcept                          {return status() == working;}

		/*
			Get smart pointers to the object.
			shared_ptr derived from these methods will block the object's destruction.

			life_lock's primary use is creating weak_ptr; use lock() only with care.
		*/
		template<class T> std::weak_ptr  <T> weak(T *ptr) const noexcept    {return _weak  (ptr);}
		template<class T> std::shared_ptr<T> lock(T *ptr) const noexcept    {return _shared(ptr);}

		/*
			Query the status of the life_lock.
				empty -- the life_lock is in an uninitialized state.
		*/
		status_t status() const noexcept
		{
			switch (_status.load(std::memory_order_acquire))
			{
			case retired: return retired;
			case expired: return expired;
			default: return _ref.use_count() ? working : empty;
			}
		}

		/*
			Release the life_lock's original reference.
			Returns the shared reference. which is safe to discard.

			This function enables remaining references to expire,
			and may avoid waiting when the life_lock is destroyed.
		*/
		template<class T = void>
		std::shared_ptr<T> retire(T *referent = nullptr) noexcept    {return std::shared_ptr<T>(_retire(), referent);}

		/*
			Destroy the reference, waiting for any shared_ptr to expire.
				Afterward, state() will be empty.

			Return value indicates whether waiting was necessary.
		*/

		size_t destroy()
		{
			size_t n=0;
			switch (status())
			{
			default:
			case working: retire();                     LIFE_LOCK_FALLTHROUGH;
			case retired: n=_await_expiration(_status); LIFE_LOCK_FALLTHROUGH;
			case expired: _finalize();                  LIFE_LOCK_FALLTHROUGH;
			case empty:   return n;
			}
		}


	protected:
		// life_lock cannot be copied (which could cause deadlock)
		life_lock           (const life_lock &o) noexcept = delete;
		life_lock& operator=(const life_lock &o) noexcept = delete;

		// life_lock cannot be moved (this would break references)
		life_lock           (life_lock &&o) noexcept = delete;
		life_lock& operator=(life_lock &&o) noexcept = delete;

		using _status_word = std::atomic<uintptr_t>;

		struct _deleter
		{
			void operator()(_status_word *lock) const noexcept
			{
				lock->store(expired, std::memory_order_release);
#if LIFE_LOCK_CPP20
				lock->notify_one();
#endif
			}
		};

		struct _internal_tag {};

#if !LIFE_LOCK_COMPRESS
		/*
			Standard (safe) implementation.
				Typically 3 words (12 or 24 bytes) in size.
		*/
		std::shared_ptr<_status_word> _ref;
		_status_word                  _status = empty;

		life_lock(_internal_tag, std::shared_ptr<_status_word> ptr = {})
			:
			_ref(std::move(ptr)), _status(_ref ? working : empty) {}

		template<class T> std::weak_ptr  <T> _weak  (T *p) const noexcept    {return _shared(p);}
		template<class T> std::shared_ptr<T> _shared(T *p) const noexcept    {return std::shared_ptr<T>(_ref, p);}

		std::shared_ptr<_status_word> _retire() noexcept
		{
			decltype(_ref) ref = std::move(_ref);
			if (ref) _status.store(retired, std::memory_order_release);
			return ref;
		}
		void _finalize() noexcept    {_status.store(empty, std::memory_order_release);}
		void _destruct() noexcept    {}
#else
		/*
			Low-memory implementation.
				Relies on assumption that shared_anchor's first word
				(typically null or a pointer) never has value 0 or 1.
		*/
		union
		{
			shared_anchor _ref;
			_status_word  _status;
		};

		life_lock(_internal_tag, std::shared_ptr<_status_word> ptr = {})
			:
			_ref(std::move(ptr)) {}

		template<class T> std::weak_ptr  <T> _weak  (T *p) const noexcept    {return is_working() ?_ref.get_weak  (p) : std::weak_ptr<T>  {};}
		template<class T> std::shared_ptr<T> _shared(T *p) const noexcept    {return is_working() ?_ref.get_shared(p) : std::shared_ptr<T>{};}

		std::shared_ptr<void> _retire()
		{
			std::shared_ptr<void> ref;
			if (status() == working)
			{
				ref = _ref.release<void>(nullptr);
				_ref.~shared_anchor();
				_status.store(retired, std::memory_order_relaxed);
			}
			return ref;
		}
		void _finalize() noexcept    {new (&_ref) shared_anchor();}
		void _destruct() noexcept    {switch (_status.load()) {case retired: case expired: break; default: _ref.~shared_anchor();} }
#endif

		static size_t _await_expiration(_status_word &lock)
		{
			size_t n = 0;

			// 1: only wait if the lock's state is initially "retired".
			switch (lock.load(std::memory_order_acquire))
			{
			default: throw std::runtime_error("Invalid state for awaiting expiration");
			case empty: case expired: return n;
			case retired: break;
			}
			++n;

#if LIFE_LOCK_CPP20
			// Use standard C++ library's notify mechanism.
			lock.wait(retired, std::memory_order_acquire);
#else

			// 2: Spin for a short time.
			while (n++ < LIFE_LOCK_SPIN_COUNT)
				if (lock.load(std::memory_order_acquire) == expired) return n;

			// 3: Wait increasing periods of time.
			size_t wait_usec = 1;
			while (lock.load(std::memory_order_acquire) != expired)
			{
				++n;
				std::this_thread::sleep_for(std::chrono::microseconds(wait_usec));
				wait_usec *= 2;
				if (wait_usec > LIFE_LOCK_SLEEP_MAX_USEC) wait_usec = LIFE_LOCK_SLEEP_MAX_USEC;
			}
#endif
			return n;
		}
	};

	/*
		A life_lock applying to itself.
			Can be used as a member of the protected object, with precautions:
			the life_lock must be destroyed before any protected member variables.

		Differences:
			Default constructs to an initialized lock.
			Copy construction is allowed, but produces a new lock (not a clone).
	*/
	class life_lock_self : public life_lock
	{
	public:
		life_lock_self()                          : life_lock(this) {}
		life_lock_self(const life_lock_self&)     : life_lock(this) {}
	};


	// Placeholder for life_locked constructor
	enum life_locked_empty_t    {life_locked_empty};

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
		// Construct with T's constructor arguments, or T() for the default constructor.
		template<typename... Args>
		life_locked(Args&&... args)    : _lock(new (_t()) T (std::forward<Args>(args)...)) {}
		
		// Construct life_locked in an empty/destroyed state.
		life_locked(life_locked_empty_t)    {}

		// Wait until all shared_ptr have expired and destroy the contained object.
		~life_locked()    {destroy();}
		void retire()     {if (_lock) {_lock.retire();}}
		void destroy()    {if (_lock) {_lock.destroy(); _t()->~T();}}
		void reset()      {if (_lock) {_lock.destroy(); _t()->~T();}}  // "reset" alias for consistency with std::optional

		// Get weak pointer
		std::weak_ptr        <T>   weak()       noexcept    {return _lock.weak(raw_ptr());}
		std::weak_ptr  <const T>   weak() const noexcept    {return _lock.weak(raw_ptr());}
		std::shared_ptr<      T>   lock()       noexcept    {return _lock.lock(raw_ptr());}
		std::shared_ptr<const T>   lock() const noexcept    {return _lock.lock(raw_ptr());}
		operator std::weak_ptr      <T>()       noexcept    {return _lock.weak(raw_ptr());}
		operator std::weak_ptr<const T>() const noexcept    {return _lock.weak(raw_ptr());}

		// Check on contained value
		bool has_value()         const noexcept    {return _lock;}
		explicit operator bool() const noexcept    {return _lock;}
		T       &value()       noexcept            {return *raw_ptr();}
		const T &value() const noexcept            {return *raw_ptr();}

		// Access the contained object.
		T       *raw_ptr   ()       noexcept    {return _lock ? _t() : nullptr;}
		const T *raw_ptr   () const noexcept    {return _lock ? _t() : nullptr;}
		T       *operator->()       noexcept    {return  raw_ptr();}
		const T *operator->() const noexcept    {return  raw_ptr();}
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
