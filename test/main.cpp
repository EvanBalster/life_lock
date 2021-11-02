#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <iomanip>
#include <cstdint>
#include <vector>

#include <atomic>
#include <chrono>
#include <thread>


#define LIFE_LOCK_COMPRESS 1

#include <life_lock.h>

/*
	M.E. O'Neill's wonderful Permuted Congruential Generator.
*/
struct PCG_Rand
{
	PCG_Rand(uint64_t seed, uint64_t inc = 1)
		: _state(seed), _inc(inc|1) {advance();}

	uint32_t operator()() noexcept    {uint64_t old = _state; advance(); return _output(old);}
	uint32_t peek() const noexcept    {return _output(_state);}
	inline void advance() noexcept    {_state = _state * 6364136223846793005ULL + _inc;}

protected:
	inline static uint32_t _output(const uint64_t state) noexcept
	{
		// Calculate output function (XSH RR)
		uint32_t xorshifted = uint32_t(((state >> 18u) ^ state) >> 27u);
		uint32_t rot = uint32_t(state >> 59u);
		return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
	}

	uint64_t _state;
	uint64_t _inc = 1; // Should be odd
};


class Receiver
{
public:
	struct hash_test : private std::hash<uint32_t>
	{
		static const uint32_t MASK = 0xFF;
		uint32_t pattern;

		hash_test()                  : pattern(0) {}
		hash_test(uint32_t _pattern) : pattern(_pattern&MASK) {}

		size_t hash(uint32_t v) const    {return std::hash<uint32_t>::operator()(v);}
		bool operator()(uint32_t v) const    {return (hash(v) & MASK) == pattern;}
	};

public:
	Receiver(hash_test test)
		: _life(this), _test(test)
	{
		_itemCount = 0;
		
		_no_more_submits = false;
		
		std::stringstream ss;
		ss << std::hex << "rcv@" << this << "/" << _test.pattern;
		name = ss.str();
		
		std::cout << name << ": created\n" << std::flush;
	}

	~Receiver()
	{
		std::cout << name << ": destroying...\n" << std::flush;
		
		// It's all over!!
		_life.destroy();
		_no_more_submits = true;

		// Verify...
		size_t
			final_submits = _itemCount.exchange(CAPACITY),
			final_count = std::min<size_t>(final_submits, CAPACITY),
			pass_count = 0;
			
		std::cout << name << ": " << final_submits << " were submitted\n" << std::flush;

		// Verify items
		for (size_t i = 0; i < final_count; ++i)
		{
			if (_test(_items[i])) ++pass_count;
		}
		std::cout << name << ": " << pass_count << "/" << final_count << " passed" << std::endl;
			
		if (pass_count < final_count)
			std::cout << "FAIL (hash): " << (final_count-pass_count) << " hashes" << std::endl;

		// Ensure no items are submitted after destruction
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (_itemCount != CAPACITY)
		{
			std::cout << "FAIL (life_lock): received " << (_itemCount-CAPACITY) << " items after destruction" << std::endl;
		}
		
		std::cout << name << ": finished" << std::endl;
	}
	
	
	std::weak_ptr<Receiver> get_weak()    {return _life.weak(this);}
	
	
	// Is the receiver full?  Safe to call from anywhere.
	bool full() const    {return _itemCount >= CAPACITY;}
	

	// Test, safe to access from callers
	hash_test test() const    {return _test;}

	// Asynchronous callback: get data from sender
	bool async_submit(uint32_t item)
	{
		if (_no_more_submits)
		{
			std::cout << "FAIL: item submitted after receiver's life_lock destroyed" << std::endl;
		}
		
		size_t index = _itemCount++;
		if (index < CAPACITY)
		{
			_items[index] = item;
			return true;
		}
		else return false;
	}

protected:
	std::string name;

	const hash_test _test;
	
	edb::life_lock _life;

	enum {CAPACITY = 32768};
	uint32_t            _items[CAPACITY];
	std::atomic<size_t> _itemCount;
	
	std::atomic<bool>   _no_more_submits;
};

/*
	Does work and sends results to the receiver.
*/
struct Sender
{
public:
	Sender(std::weak_ptr<Receiver> &&_receiver, uint64_t seed) :
		receiver(std::move(_receiver)), random(seed)
	{
		//std::cout << "Sender @" << this << ": created" << std::endl;
		
		// Grab the receiver's hash test.
		{
			auto rcv = receiver.lock();
			if (rcv)
			{
				test = rcv->test();
			}
			else
			{
				std::cout << "ODDITY: receiver expired before sender could start working (this is not necessarily an error)" << std::endl;
			}
		}
	}
	
	~Sender()
	{
		//std::cout << "Sender @" << this << ": completed "
		//	<< n_submit << "/" << n_attempt << " hashes" << std::endl;
	}

	void run()
	{
		while (true)
		{
			// Do some pointless, environmentally destructive work
			uint32_t solution;
			do {solution = random(); ++n_attempt;} while (!test(solution));
			
			++n_submit;
			
			// Lock the receiver, stopping our work if it no longer exists.
			auto rcv = receiver.lock();
			if (!rcv) return;
			
			auto use_count = rcv.use_count();
			rcv->async_submit(solution);
		}
	}


protected:
	std::weak_ptr<Receiver> receiver;
	Receiver::hash_test     test;
	PCG_Rand                random;
	
	size_t n_attempt = 0, n_submit = 0;
};


int main(int argc, char **argv)
{
	auto RunReceiver = [](uint64_t seed)
	{
		auto workStart = std::chrono::steady_clock::now();
	
		PCG_Rand rand(seed);
		
		auto RunSender = [](std::weak_ptr<Receiver> receiver, uint64_t seed)
		{
			Sender sender(std::move(receiver), seed);
			sender.run();
		};
		
		do
		{
			std::thread sendThreads[8];
			
			// Do the bad thing
			std::weak_ptr<Receiver> test_ptr;
			std::shared_ptr<Receiver> deadlocker;
			
			// Lifespan of receiver...
			{
				Receiver receiver(rand());
				test_ptr = receiver.get_weak();
				
				// The bad thing
				//deadlocker = test_ptr.lock();
				
				// Create sender threads
				for (auto &thread : sendThreads)
					thread = std::thread(RunSender, receiver.get_weak(), rand());
				
				// Wait to fill up
				while (!receiver.full())
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
				
			for (auto &thread : sendThreads)
				thread.join();
		}
		while (std::chrono::steady_clock::now() < workStart + std::chrono::seconds(30));
	};
	
	
	PCG_Rand rand(std::chrono::system_clock::now().time_since_epoch().count());
	
	std::thread rcvThreads[8];
	for (auto &thread : rcvThreads)
		thread = std::thread(RunReceiver, rand());
		
	for (auto &thread : rcvThreads)
		thread.join();
		
	std::cout << "Test completed" << std::endl;
}
