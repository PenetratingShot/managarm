#pragma once

#include "types.hpp"
#include <physical-buddy.hpp>

namespace thor {

struct SkeletalRegion {
public:
	static void initialize();

	static SkeletalRegion &global();

	// TODO: make this private
	SkeletalRegion() = default;

	SkeletalRegion(const SkeletalRegion &other) = delete;
	
	SkeletalRegion &operator= (const SkeletalRegion &other) = delete;

	void *access(PhysicalAddr physical);
};

class PhysicalChunkAllocator {
	typedef frigg::TicketLock Mutex;
public:
	PhysicalChunkAllocator();
	
	void bootstrapRegion(PhysicalAddr address,
			int order, size_t numRoots, int8_t *buddyTree);

	PhysicalAddr allocate(size_t size, int addressBits = 64);
	void free(PhysicalAddr address, size_t size);

	size_t numUsedPages();
	size_t numFreePages();

private:
	Mutex _mutex;

	struct Region {
		PhysicalAddr physicalBase;
		PhysicalAddr regionSize;
		BuddyAccessor buddyAccessor;
	};

	Region _allRegions[8];
	int _numRegions = 0;

	size_t _usedPages = 0;
	size_t _freePages = 0;
};

extern frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;

} // namespace thor
