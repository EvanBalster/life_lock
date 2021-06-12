#include <iostream>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <life_lock.h>


struct PCG_Rand // M.E. O'Neill's wonderful permuted congruential generator
{
	PCG_Rand(uint64_t seed, uint64_t inc = 1)    : _state(seed), _inc(inc|1) {advance();}

	uint32_t operator()() noexcept    {uint64_t old = _state; advance(); return _output(old);}
	uint32_t peek() const noexcept    {return _output(_state);}
	inline void advance() noexcept    {_state = _state * 6364136223846793005ULL + _inc;}

protected:
	inline static uint32_t _output(const uint64_t state) noexcept
	{
		// Calculate output function (XSH RR)
		uint32_t xorshifted = ((state >> 18u) ^ state) >> 27u;
		uint32_t rot = state >> 59u;
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
		static const uint32_t MASK = 0xFFF;
		uint32_t pattern;

		hash_test(uint32_t _pattern) : pattern(_pattern&MASK) {}

		size_t hash(uint32_t v) const    {return std::hash<uint32_t>::operator()(v);}
		bool operator()(uint32_t v) const    {return (hash(v) & MASK) == pattern;}
	};

public:
	Receiver(hash_test test) : _life(this), _test(test) {}

	~Receiver()
	{
		// It's all over!!
		_life.destroy();

		// Verify...
		size_t final_count = _itemCount, pass_count = 0;

		// Verify items
		for (size_t i = 0; i < final_count; ++i)
		{
			if (!_test(_items[i]))
				++pass_count;
			else
				std::cout << "TEST FAIL: " << _test.pattern
				<< "[" << i << "] = " << _items[i]
				<< " -> " << _test.hash(_items[i])
				<< std::endl;
		}
		std::cout << "Receiver test " << _test.pattern << ": "
			<< pass_count << "/" << final_count << " passed"
			<< std::endl;

		std::this_thread::sleep_for(std::chrono::seconds(5));
	}

	hash_test test() const    {return _test;}

	// Asynchronous callback: get data from sender
	void async_submit(uint32_t item)
	{
		uint32_t index = _itemCount++;
		if (index < CAPACITY) _items[index] = item;
	}

protected:
	edb::life_lock _life;
	hash_test      _test;

	enum {CAPACITY = 4096};
	uint32_t            _items[CAPACITY];
	std::atomic<size_t> _itemCount = 0;
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
		thread = std::thread(&Sender::run, this);
	}

	void run()
	{
		std::hash<uint32_t> hash;
		uint32_t suffix;

		while (true)
		{
			// Pointless, environmentally destructive work
			uint32_t solution;
			while (true)
			{
				solution = random();
				if (hash(solution)) ;

			}
		}
	}


protected:
	std::weak_ptr<Receiver> receiver;
	PCG_Rand                random;

	std::thread thread;

};


void main(int argc, char **argv)
{
	
}
