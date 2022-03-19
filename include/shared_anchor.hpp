#pragma once

#include <memory>


/*
	Classes based upon shared_ptr and weak_ptr but with
	reduced functionality: they hold a strong or weak reference
	but do not expose any pointer to the referent.
	
	These limitations allow an implementation with a smaller
	memory footprint (the size of one pointer) on most platforms.

	Enable this optimization using #define SHARED_PTR_HACKS.
	Compression relies on undefined behavior which nevertheless
	is quite likely to be reliable on most compilers/environments.
*/

#ifndef SHARED_PTR_HACKS
	#define SHARED_PTR_HACKS 0
#endif


namespace edb
{
	// These typedefs may be modified, eg, to use boost::shared_ptr.
	template<class Y> using shared_ptr = std::shared_ptr<Y>;
	template<class Y> using weak_ptr   = std::weak_ptr  <Y>;


	class shared_anchor;
	class weak_anchor;


#if !SHARED_PTR_HACKS

	/*
		REFERENCE IMPLEMENTATION:

		These classes simply wrap shared_ptr and weak_ptr,
			providing the same reduced API as the "shrink" versions.
			They are reliable with any compliant C++11 environment.
		
		They require as much memory as a smart pointer, and incur some
			additional run-time cost when generating weak pointers.
	*/

	namespace detail
	{
		template<template<class> class T>
		class anchor_base
		{
		public:
			// Typical constructors and destructor.
			constexpr         anchor_base()              noexcept    = default;
			template<class Y> anchor_base(const T<Y> &t) noexcept    : _cb(t) {}
			template<class Y> anchor_base(T<Y>      &&t) noexcept    : _cb(std::move(t)) {}
			~anchor_base()                               noexcept    {}

			// These functions are equivalent to the ones in std::shared_ptr and std::weak_ptr.
			void                                    reset()                                      noexcept    {_cb.reset();}
			long                                    use_count   ()                         const noexcept    {return _cb.use_count();}
			template<template<class> class T2> bool owner_before(const anchor_base<T2> &p) const noexcept    {return _cb.owner_before(p._cb);}
			template<class Y>                  bool owner_before(const shared_ptr<Y>   &p) const noexcept    {return _cb.owner_before(p);}
			template<class Y>                  bool owner_before(const weak_ptr  <Y>   &p) const noexcept    {return _cb.owner_before(p);}

		protected:
			T<void> _cb;
		};
	}

	class shared_anchor :
		public detail::anchor_base<std::shared_ptr>
	{
	public:
		constexpr shared_anchor() noexcept = default;
		using anchor_base::anchor_base;

		// Create an explicitly empty shared_anchor.
		constexpr shared_anchor(nullptr_t) noexcept    {}

		~shared_anchor() noexcept                      {}


		// Create a shared_ptr or weak_ptr with the givens referent.
		template<class Y> shared_ptr<Y> get_shared(Y *referent) const noexcept        {return shared_ptr<Y>(_cb, referent);}
		template<class Y> weak_ptr  <Y> get_weak  (Y *referent) const noexcept        {return get_shared(referent);}

		// Release and return the strong reference.
		template<class Y> shared_ptr<Y> release   (Y *referent)       noexcept        {return shared_ptr<Y>(std::move(_cb), referent);}


		/*
			The following functions are implemented according to the specification of std::shared_ptr:
				operator bool
				reset
				use_count
				owner_before
		*/
		explicit operator bool() const noexcept    {return bool(_cb);}
	};

	class weak_anchor :
		public detail::anchor_base<std::weak_ptr>
	{
	public:
		constexpr weak_anchor() noexcept = default;
		using anchor_base::anchor_base;

		// Create a weak_anchor from a shared_anchor or shared_ptr
		weak_anchor                  (const shared_anchor &t) noexcept           : anchor_base(t.get_weak<void>(nullptr)) {}
		template<class Y> weak_anchor(const shared_ptr<Y> &t) noexcept           : anchor_base(shared_ptr<void>(t)) {}

		~weak_anchor() noexcept                                                  {}


		// Create a shared_ptr or weak_ptr with the given referent.
		template<class Y> shared_ptr<Y> lock    (Y *referent) const noexcept     {return shared_ptr<Y>(_cb.lock(), referent);}
		template<class Y> weak_ptr  <Y> get_weak(Y *referent) const noexcept     {return get_shared(referent);}

		// Release and return the weak reference.
		template<class Y> weak_ptr  <Y> release (Y *referent)       noexcept     {auto r=get_weak(referent); reset(); return r;}


		/*
			The following functions are implemented according to the specification of std::weak_ptr:
				operator bool
				reset
				use_count
				owner_before
		*/
		bool expired() const noexcept    {return _cb.expired();}
	};

	#if 0
	/*
		An anchor which can switch between weak and shared.
			Exposes the same API as weak_anchor, plus strength management functions.

		This class uses some minor hacks to affect reference count without undue size footprint.
	*/
	class weak_or_shared_anchor : public weak_anchor
	{
	public:
		// Create an empty weak_anchor.
		constexpr weak_or_shared_anchor() noexcept = default;

		// Create from a shared_anchor.
		weak_or_shared_anchor(const shared_anchor &t) noexcept                                     : weak_anchor(_init_strong(t._cb)) {}
		weak_or_shared_anchor(const weak_anchor   &t) noexcept                                     : weak_anchor(t) {}

		// Create a weak_anchor from a shared_ptr or weak_ptr.
		template<class Y> weak_or_shared_anchor(const shared_ptr<Y> &t) noexcept                   : _cb(shared_ptr<void>(t)) {}
		template<class Y> weak_or_shared_anchor(const weak_ptr  <Y> &t) noexcept                   : _cb(shared_ptr<void>(t.lock(), nullptr)) {}

		~weak_or_shared_anchor() noexcept                                                          {_set_strong(false);}


		// Methods for releasing the reference.
		template<class Y> weak_ptr  <Y> release (Y *referent) noexcept    {auto r=get_weak(referent); reset(); return r;}
		void                            reset()               noexcept    {_set_strong(false); _cb.reset();}


		/*
			Manage the strength of this anchor.
		*/
		bool is_strong() const noexcept    {return _is_strong();}

		void make_strong() noexcept    {_set_strong(true);}
		void make_weak  () noexcept    {_set_strong(false);}


	private:
		friend class shared_anchor;
		weak_ptr<void> _cb;

		using _spv = std::shared_ptr<void>;

		std::weak_ptr<void> _init_weak  (std::shared_ptr<void> p)    {             return _spv(std::move(p), nullptr);}
		std::weak_ptr<void> _init_strong(std::shared_ptr<void> p)    {_add_ref(p); return _spv(std::move(p), (void*) uintptr_t(1));}

		// HACKS: use a stack buffer to clone shared_ptr in order to retain and release.
		static void _add_ref (_spv &sp)    {alignas(_spv) char buf[sizeof(_spv)]; new (buf) _spv(sp);}
		static void _drop_ref(_spv &sp)    {alignas(_spv) char buf[sizeof(_spv)]; auto spb = ((char*)&sp); std::memcpy(buf, spb, sizeof(_spv)); sp.~shared_ptr(); std::memcpy(spb, buf, sizeof(_spv));}

		bool _is_strong () const noexcept         {return uintptr_t(_cb.lock().get()) & 1;}
		_spv _set_strong(bool strong) noexcept
		{
			auto sp = _cb.lock();
			if (!sp) return;
			auto r = sp.get();
			bool is = uintptr_t(r)&1;
			if (is == strong) return;
			if (strong) _add_ref(sp); else _drop_ref(sp);
			sp = _spv(std::move(sp), (void*) uintptr_t(strong));
		}
	};
	#endif


	template<typename Y>
	inline bool shared_ref_unit_test() noexcept    {return true;}


#else

	/*
		EXPERIMENTAL IMPLEMENTATION:

		These classes provide the same functionality as those above,
			but save space and time by relying on undefined behavior.
			The shared_anchor and weak_anchor here are the size of a pointer.
		
		They appear to work in most C++11 environments, but should be
			tested prior to use by calling shared_ref_unit_test().
	*/

	namespace detail
	{
		/*
			This class encapsulates shared pointer hacks.
		*/
		struct alignas(shared_ptr<void>) alignas(weak_ptr<void>) smart_ptr_dissect_buffer
		{
			using _buf = smart_ptr_dissect_buffer;
			static const size_t SIZE_IN_WORDS = (sizeof(shared_ptr<void>)+sizeof(void*)-1)/sizeof(void*);
			void *v[SIZE_IN_WORDS];

			// Detect where this platform locates the control block pointer. 
			//    Optimizing compilers may reduce this to a constant.
			static unsigned _cbp() noexcept    {_buf b = {{(void*)0x1111,(void*)0x2222}}; return ((shared_ptr<void>*)b.v)->get() != b.v[1];}

			void *control_block_ptr() const noexcept    {return v[_cbp()];}

			template<typename Y = void>
			_buf &set(void *control_block, Y *referent=0)    {v[_cbp()] = control_block; v[!_cbp()] = referent; return *this;}

			template<class T> T &view() noexcept
			{
				static_assert((sizeof(T) == sizeof(_buf)) && (alignof(T) <= alignof(_buf)),
					"Abnormal smart pointer size; SHARED_PTR_HACKS may be unusable in this environment");
				return *(T*)v;
			}
		};

		/*
			POD base class for small shared_anchor and weak_anchor.
				WARNING: incorrect use can corrupt reference counts.
		*/
		struct control_block_ref
		{
		public:
			using _buf = smart_ptr_dissect_buffer;

			// Acquire from a shared_ptr or weak_ptr.
			template<class Y> void _retain_shared(      shared_ptr<Y>&&p) noexcept    {_buf b; new ((void*)&b) shared_ptr<Y>(std::move(p)); _cb = b.control_block_ptr();}
			template<class Y> void _retain_weak  (        weak_ptr<Y>&&p) noexcept    {_buf b; new ((void*)&b)   weak_ptr<Y>(std::move(p)); _cb = b.control_block_ptr();}
			template<class Y> void _retain_shared(const shared_ptr<Y> &p) noexcept    {_buf b; new ((void*)&b) shared_ptr<Y>(p);            _cb = b.control_block_ptr();}
			template<class Y> void _retain_weak  (const shared_ptr<Y> &p) noexcept    {_buf b; new ((void*)&b)   weak_ptr<Y>(p);            _cb = b.control_block_ptr();}
			template<class Y> void _retain_weak  (const   weak_ptr<Y> &p) noexcept    {_buf b; new ((void*)&b)   weak_ptr<Y>(p);            _cb = b.control_block_ptr();}

			// Unshrink to a shared_ptr or weak_ptr.
			template<class Y> shared_ptr<Y> &_view_shared(_buf &b, Y *referent=0) const    {return b.set(_cb, referent).view<shared_ptr<Y>>();}
			template<class Y> weak_ptr  <Y> &_view_weak  (_buf &b, Y *referent=0) const    {return b.set(_cb, referent).view<  weak_ptr<Y>>();}

			// Unshrink and transfer a shared_ptr or weak_ptr.
			template<class Y> shared_ptr<Y> _release_shared(Y *referent)    {_buf b; shared_ptr<Y> p = std::move(b.set(_cb, referent).view<shared_ptr<Y>>()); _cb=0; return p;}
			template<class Y> weak_ptr  <Y> _release_weak  (Y *referent)    {_buf b; weak_ptr  <Y> p = std::move(b.set(_cb, referent).view<  weak_ptr<Y>>()); _cb=0; return p;}

			// Release as a shared_ptr or weak_ptr.
			template<class Y = void> void _release_shared() noexcept   {_buf b; b.set(_cb).view<shared_ptr<Y>>().~shared_ptr(); _cb = nullptr;}
			template<class Y = void> void _release_weak  () noexcept   {_buf b; b.set(_cb).view<  weak_ptr<Y>>().~weak_ptr();   _cb = nullptr;}
			

			void *_cb; // Control block
		};
	}

	/*
		shared_anchor is a shared_ptr without the pointer.
			It holds ownership but doesn't provide access to the object.
			Consequently, on many platforms it can be smaller ("compressed").
	*/
	class shared_anchor : private detail::control_block_ref
	{
	private:
		friend class weak_anchor;
		using _ref = detail::control_block_ref;
		void _release()    {return _release_shared();}


	public:
		// Create an empty shared_anchor.
		constexpr shared_anchor()          noexcept    : _ref{} {}
		constexpr shared_anchor(nullptr_t) noexcept    : _ref{} {}
		
		// Create a shared_anchor from a shared_ptr.
		template<class Y> shared_anchor(const shared_ptr<Y> &t) noexcept    {_retain_shared(t);}
		template<class Y> shared_anchor(      shared_ptr<Y>&&t) noexcept    {_retain_shared(std::move(t));}

		// Release the held reference.
		~shared_anchor() noexcept                                           {_release();}


		// Produce pointers using the same management
		template<class Y> shared_ptr<Y> get_shared(Y *referent) const noexcept    {_buf b; return _view_shared<Y>(b,referent);}
		template<class Y> weak_ptr  <Y> get_weak  (Y *referent) const noexcept    {_buf b; return _view_shared<Y>(b,referent);}


		// Release and return the strong reference.
		template<class Y> shared_ptr<Y> release(Y *referent) noexcept    {return _release_shared<Y>(referent);}


		// These functions are equivalent to the ones in std::shared_ptr.
		void                   reset()                                    noexcept    {_release();}
		explicit operator      bool()                               const noexcept    {return bool(_cb);}
		long                   use_count   ()                       const noexcept    {_buf b; return _view_shared<void>(b).use_count();}
		bool                   owner_before(const shared_anchor &p) const noexcept    {_buf b,c; return _view_shared<void>(b).owner_before(p._view_shared<void>(c));}
		bool                   owner_before(const weak_anchor   &p) const noexcept    {_buf b,c; return _view_shared<void>(b).owner_before(reinterpret_cast<const _ref&>(p)._view_shared<void>(c));}
		template<class Y> bool owner_before(const shared_ptr<Y> &p) const noexcept    {_buf b; return _view_shared(b).owner_before(p);}
		template<class Y> bool owner_before(const weak_ptr  <Y> &p) const noexcept    {_buf b; return _view_shared(b).owner_before(p);}


		// Move/copy with correct reference management
		shared_anchor                 (shared_anchor&&o) noexcept    {            _cb = o._cb; o._cb = 0;}
		shared_anchor& operator=      (shared_anchor&&o) noexcept    {_release(); _cb = o._cb; o._cb = 0; return *this;}
		shared_anchor           (const shared_anchor &o) noexcept    {            _buf b; _retain_shared(o._view_shared<void>(b));}
		shared_anchor& operator=(const shared_anchor &o) noexcept    {_release(); _buf b; _retain_shared(o._view_shared<void>(b)); return *this;}
	};



	class weak_anchor : private detail::control_block_ref
	{
	private:
		using _ref = detail::control_block_ref;
		void _release()    {return _release_weak();}

	public:
		// Create an empty weak_anchor.
		constexpr weak_anchor() noexcept    : _ref{} {}

		// Create a weak_anchor from a shared_anchor.
		weak_anchor(const shared_anchor &t) noexcept                                        {_buf b; _retain_weak(t._view_shared<void>(b));}

		// Create a weak_anchor from a shared_ptr or weak_ptr.
		template<class Y> weak_anchor(const shared_ptr<Y> &t) noexcept                   {_retain_weak(t);}
		template<class Y> weak_anchor(const weak_ptr  <Y> &t) noexcept                   {_retain_weak(t);}
		template<class Y> weak_anchor(      weak_ptr  <Y>&&t) noexcept                   {_retain_weak(std::move(t));}

		~weak_anchor() noexcept                                                          {_release();}


		// Create a shared_ptr or weak_ptr with the given referent.
		template<class Y> shared_ptr<Y> lock    (Y *referent) const noexcept          {_buf b; return _view_weak(b,referent).lock();}
		template<class Y> weak_ptr  <Y> get_weak(Y *referent) const noexcept          {_buf b; return _view_weak(b,referent);}

		// Release and return the weak reference.
		template<class Y> weak_ptr  <Y> release (Y *referent) const noexcept          {return _release_weak(referent);}


		// These functions are equivalent to the ones in std::weak_ptr.
		void                   reset()                                    noexcept    {_release_shared();}
		bool                   expired     ()                       const noexcept    {return bool(_cb);}
		long                   use_count   ()                       const noexcept    {_buf b; return _view_weak<void>(b).use_count();}
		bool                   owner_before(const shared_anchor &p) const noexcept    {_buf b,c; return _view_weak<void>(b).owner_before(p._view_shared<void>(c));}
		bool                   owner_before(const weak_anchor   &p) const noexcept    {_buf b,c; return _view_weak<void>(b).owner_before(p._view_weak  <void>(c));}
		template<class Y> bool owner_before(const shared_ptr<Y> &p) const noexcept    {_buf b; return _view_weak(b).owner_before(p);}
		template<class Y> bool owner_before(const weak_ptr  <Y> &p) const noexcept    {_buf b; return _view_weak(b).owner_before(p);}


		// Move/copy with correct reference management
		weak_anchor                 (weak_anchor&&o) noexcept    {            _cb = o._cb; o._cb = 0;}
		weak_anchor& operator=      (weak_anchor&&o) noexcept    {_release(); _cb = o._cb; o._cb = 0; return *this;}
		weak_anchor           (const weak_anchor &o) noexcept    {            _buf b; _retain_weak(o._view_weak<void>(b));}
		weak_anchor& operator=(const weak_anchor &o) noexcept    {_release(); _buf b; _retain_weak(o._view_weak<void>(b)); return *this;}
	};

#endif
}
