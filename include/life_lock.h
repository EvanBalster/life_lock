#pragma once


#include <memory>
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
	
	The other class defined here, shared_retainer, is used to implement life_lock.
	
	This header was designed for use with asynchronous I/O delegates, allowing observers
		to be destroyed at will while protecting against concurrent access by callbacks.
		The one-time lock mechanism used here is much more lightweight than a mutex.
		In a typical application, the lock will only rarely create a delay.
*/


/*
	When defined to 1, some hacks are employed to shave the size of these classes down
		to a single pointer (typically saving 4-8 bytes wherever they are used).
*/
#ifndef LIFE_LOCK_COMPRESSED
	#define LIFE_LOCK_COMPRESSED 0
#endif


namespace edb
{
	/*
		A non-template type that holds the ownership part of a shared_ptr.
			It does not provide the pointer functionality of shared_ptr.
			Consequently, on many platforms it can be smaller ("compressed").
	*/
	class shared_retainer
	{
	public:
		shared_retainer()                                            noexcept     {}
		template<typename T> shared_retainer(std::shared_ptr<T> &&t) noexcept     {_init(std::move(t);}

		// Release the held reference.
		~shared_retainer() noexcept                                               {_clear();}
		void reset()       noexcept                                               {_clear();}

		// Check if a reference is held.
		explicit operator bool() const noexcept                                   {return bool(_cb);}

		// Produce pointers using the same management
		template<class T> std::shared_ptr<T> get_shared(T *obj) const noexcept    {buf sp; return _view<T>(obj, sp);}
		template<class T> std::shared_ptr<T> get_weak  (T *obj) const noexcept    {buf sp; return _view<T>(obj, sp);}

		// Move/copy
		shared_retainer                (shared_retainer &&o) noexcept    {_cb = std::move(o._cb); o._clear();}
		shared_retainer& operator=     (shared_retainer &&o) noexcept    {_cb = std::move(o._cb); o._clear(); return *this;}
		shared_retainer           (const shared_retainer &o) noexcept    {          _init(o.get_shared<shared_retainer>(this));}
		shared_retainer& operator=(const shared_retainer &o) noexcept    {_clear(); _init(o.get_shared<shared_retainer>(this)); return *this;}


	private:
		template<class T> using sptr_ = std::shared_ptr<T>;
		struct buf     {void *v[sizeof(sptr_<void>)/sizeof(void*)];};
#if LIFE_LOCK_COMPRESSED // Memory saving hack for typical stdlib implementations
		uintptr_t _cb = 0;
		static_assert(sizeof(sptr_<void>) == sizeof(void*[2]), "shared_ptr<T> not compressible on this platform");
		template<class T> void            _init(sptr_<T> &&t)          noexcept    {buf sp; new (sp.v) sptr_(std::move(t)); _cb = ((sp.v[1]==t.get()) ? uintptr_t(sp.v[0]) : uintptr_t(sp.v[1])&1);}
		template<class T> const sptr_<T> &_view(T *obj, buf &sp) const noexcept    {sp.v[!(_cb&1u)] = obj; sp.v[_cb&1u] = (void*) (_cb&~1u); return (sptr_<T>*) sp;}
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
		life_lock() noexcept                               {}

		// Construct a life_lock concerned with the given object.
		template<class T> life_lock(T *object) noexcept    : _retainer(std::shared_ptr<T>(object, unblocking_deleter{&_lock})) {}

		// Check if the life_lock is initialized.
		explicit operator bool() const noexcept            {return bool(_retainer);}

		/*
			Get a weak_ptr to the object.
				shared_ptr created from this weak_ptr will block
		*/
		template<class T> std::weak_ptr  <T> get_weak  (T *object) const noexcept    {return _retainer.get_weak  (object);}
		template<class T> std::shared_ptr<T> get_shared(T *object) const noexcept    {return _retainer.get_shared(object);}

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
			if (_retainer) do
			{
				// Place ownership of the object in a temporary shared pointer.
				auto temp_shared_ptr = _retainer.get_shared<life_lock>(this);
				_retainer.reset();

				// Repurpose this object as a lock, lock it and release the shared_ptr.
				_lock.test_and_set(std::memory_order_release);
				temp_shared_ptr.~shared_ptr();

				// Wait for the lock's release...  Spin up to 16,192 times.
				for (size_t i = 0; i < 0x4000; ++i)
					if (!_lock.test_and_set(std::memory_order_acquire)) break;

				// Wait for the lock's release...  Exponential backoff from .000001 to ~.26 seconds
				for (size_t i = 0; _lock.test_and_set(std::memory_order_acquire); ++i)
					std::this_thread::sleep_for(std::chrono::microseconds(1 << ((i<18)?i:18)));
			}
			while (0);
			_lock.clear(std::memory_order_relaxed);
		}


	protected:
		struct unblocking_deleter
		{
			std::atomic_flag *lock;
			template<class T> void operator()(T*) const noexcept    {lock->clear();}
		};
		union
		{
			shared_retainer  _retainer = shared_retainer();
			std::atomic_flag _lock;
		};
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
		~life_locked()    {reset();}
		void reset()      {if (_lock) {_lock.destroy(); raw_ptr()->~T();}}

		// Get weak pointer
		std::weak_ptr<T>      get() const noexcept    {return _lock.get_weak(raw_ptr());}
		operator std::weak_ptr<T>() const noexcept    {return _lock.get_weak(raw_ptr());}

		// Check on contained value
		bool has_value()         const noexcept    {return _lock;}
		explicit operator bool() const noexcept    {return _lock;}
		T       &value()                           {return *raw_ptr();}

		// Access raw pointer
		T *raw_ptr   () const noexcept    {return _lock ? _t() : nullptr;}
		T *operator->() const noexcept    {return raw_ptr();}
		T& operator* () const noexcept    {return *raw_ptr();}


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
		T              *_t() const noexcept     {return reinterpret_cast<T*>(_obj);}
	};
}