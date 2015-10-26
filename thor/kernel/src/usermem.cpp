
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory(Type type)
: flags(0), p_type(type), p_physicalPages(*kernelAlloc) { }

Memory::~Memory() {
	if(p_type == kTypePhysical) {
		// do nothing
	}else if(p_type == kTypeAllocated || p_type == kTypeCopyOnWrite) {
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t i = 0; i < p_physicalPages.size(); i++) {
			if(p_physicalPages[i] != PhysicalAddr(-1))
				physicalAllocator->free(physical_guard, p_physicalPages[i]);
		}
		physical_guard.unlock();
	}else{
		assert(!"Illegal memory type");
	}
}

auto Memory::getType() -> Type {
	return p_type;
}

void Memory::resize(size_t num_pages) {
	assert(p_physicalPages.size() < num_pages);
	p_physicalPages.resize(num_pages, -1);
}

void Memory::setPage(size_t index, PhysicalAddr page) {
	p_physicalPages[index] = page;
}

PhysicalAddr Memory::getPage(size_t index) {
	return p_physicalPages[index];
}

size_t Memory::numPages() {
	return p_physicalPages.size();
}

void Memory::zeroPages() {
	assert(p_type == kTypeAllocated);
	
	for(size_t i = 0; i < p_physicalPages.size(); i++) {
		PhysicalAddr page = p_physicalPages[i];
		assert(page != PhysicalAddr(-1));
		memset(physicalToVirtual(page), 0, kPageSize);
	}
}

void Memory::copyTo(size_t offset, void *source, size_t length) {
	assert(p_type == kTypeAllocated);
	
	size_t disp = 0;
	size_t index = offset / kPageSize;
	
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = p_physicalPages[index];
		assert(page != PhysicalAddr(-1));
		memcpy((uint8_t *)physicalToVirtual(page) + misalign, source, prefix);
		disp += prefix;
		index++;
	}

	while(length - disp >= kPageSize) {
		assert(((offset + disp) % kPageSize) == 0);
		PhysicalAddr page = p_physicalPages[index];
		assert(page != PhysicalAddr(-1));
		memcpy(physicalToVirtual(page), (uint8_t *)source + disp, kPageSize);
		disp += kPageSize;
		index++;
	}

	if(length - disp > 0) {
		PhysicalAddr page = p_physicalPages[index];
		assert(page != PhysicalAddr(-1));
		memcpy(physicalToVirtual(page), (uint8_t *)source + disp, length - disp);
	}
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(Type type, VirtualAddr base_address, size_t length)
: baseAddress(base_address), length(length), type(type),
		lowerPtr(nullptr), higherPtr(nullptr),
		leftPtr(nullptr), rightPtr(nullptr),
		parentPtr(nullptr), color(kColorNone), largestHole(0),
		memoryOffset(0), flags(0), writePermission(false), executePermission(false) {
	if(type == kTypeHole)
		largestHole = length;
}

Mapping::~Mapping() {
	frigg::destruct(*kernelAlloc, leftPtr);
	frigg::destruct(*kernelAlloc, rightPtr);
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(PageSpace page_space)
: p_root(nullptr), p_pageSpace(page_space) { }

AddressSpace::~AddressSpace() {
	frigg::destruct(*kernelAlloc, p_root);
}

void AddressSpace::setupDefaultMappings() {
	auto mapping = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
			0x100000, 0x7ffffff00000);
	addressTreeInsert(mapping);
}

void AddressSpace::map(Guard &guard,
		KernelUnsafePtr<Memory> memory, VirtualAddr address,
		size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert((length % kPageSize) == 0);

	Mapping *mapping;
	if((flags & kMapFixed) != 0) {
		assert((address % kPageSize) == 0);
		mapping = allocateAt(address, length);
	}else{
		mapping = allocate(length, flags);
	}
	assert(mapping != nullptr);

	mapping->type = Mapping::kTypeMemory;
	mapping->memoryRegion = KernelSharedPtr<Memory>(memory);

	uint32_t page_flags = 0;

	constexpr uint32_t mask = kMapReadOnly | kMapReadExecute | kMapReadWrite;
	if((flags & mask) == kMapReadWrite) {
		page_flags |= PageSpace::kAccessWrite;
		mapping->writePermission = true;
	}else if((flags & mask) == kMapReadExecute) {
		page_flags |= PageSpace::kAccessExecute;
		mapping->executePermission = true;
	}else{
		assert((flags & mask) == kMapReadOnly);
	}

	if(flags & kMapShareOnFork)
		mapping->flags |= Mapping::kFlagShareOnFork;

	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
	for(size_t i = 0; i < length / kPageSize; i++) {
		PhysicalAddr physical = memory->getPage(i);
		if(physical == PhysicalAddr(-1))
			continue;
		VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;
		p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
	}
	if(physical_guard.isLocked())
		physical_guard.unlock();

	*actual_address = mapping->baseAddress;
}

void AddressSpace::unmap(Guard &guard, VirtualAddr address, size_t length) {
	Mapping *mapping = getMapping(address);
	assert(mapping != nullptr);
	assert(mapping->type == Mapping::kTypeMemory);

	// TODO: allow shrink of mapping
	assert(mapping->baseAddress == address);
	assert(mapping->length == length);
	
	for(size_t i = 0; i < mapping->length / kPageSize; i++) {
		VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;
		p_pageSpace.unmapSingle4k(vaddr);
	}

	mapping->memoryRegion.reset();

	Mapping *lower_ptr = mapping->lowerPtr;
	Mapping *higher_ptr = mapping->higherPtr;
	
	if(lower_ptr && higher_ptr
			&& lower_ptr->type == Mapping::kTypeHole
			&& higher_ptr->type == Mapping::kTypeHole) {
		// grow the lower region and remove both the mapping and the higher region
		size_t mapping_length = mapping->length;
		size_t higher_length = higher_ptr->length;

		addressTreeRemove(mapping);
		addressTreeRemove(higher_ptr);
		frigg::destruct(*kernelAlloc, mapping);
		frigg::destruct(*kernelAlloc, higher_ptr);

		lower_ptr->length += mapping_length + higher_length;
		updateLargestHoleUpwards(lower_ptr);
	}else if(lower_ptr && lower_ptr->type == Mapping::kTypeHole) {
		// grow the lower region and remove the mapping
		size_t mapping_length = mapping->length;

		addressTreeRemove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		
		lower_ptr->length += mapping_length;
		updateLargestHoleUpwards(lower_ptr);
	}else if(higher_ptr && higher_ptr->type == Mapping::kTypeHole) {
		// grow the higher region and remove the mapping
		size_t mapping_length = mapping->length;

		addressTreeRemove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		
		higher_ptr->baseAddress -= mapping_length;
		higher_ptr->length += mapping_length;
		updateLargestHoleUpwards(higher_ptr);
	}else{
		// turn the mapping into a hole
		mapping->type = Mapping::kTypeHole;
		updateLargestHoleUpwards(mapping);
	}
}

bool AddressSpace::handleFault(Guard &guard, VirtualAddr address, uint32_t flags) {
	Mapping *mapping = getMapping(address);
	if(!mapping)
		return false;
	if(mapping->type != Mapping::kTypeMemory)
		return false;
	
	VirtualAddr offset = address - mapping->baseAddress;
	VirtualAddr page_vaddr = address - (address % kPageSize);
	size_t page_index = offset / kPageSize;	

	KernelUnsafePtr<Memory> memory = mapping->memoryRegion;
	if(memory->getType() == Memory::kTypeAllocated
			&& (memory->flags & Memory::kFlagOnDemand)) {
		// allocate a new page
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, 1);
		
		assert(memory->getPage(page_index) == PhysicalAddr(-1));
		memory->setPage(page_index, physical);
		
		// map the new page into the address space
		uint32_t page_flags = 0;
		if(mapping->writePermission)
			page_flags |= PageSpace::kAccessWrite;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;
		
		p_pageSpace.mapSingle4k(physical_guard, page_vaddr, physical, true, page_flags);
		physical_guard.unlock();

		return true;
	}else if(memory->getType() == Memory::kTypeCopyOnWrite) {
		// allocate a new page and copy content from the master page
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, 1);
		physical_guard.unlock();
		
		PhysicalAddr origin = memory->master->getPage(page_index);
		assert(origin != PhysicalAddr(-1)); // TODO: implement recursive copy-on-write
		memcpy(physicalToVirtual(physical), physicalToVirtual(origin), kPageSize);
		assert(memory->getPage(page_index) == PhysicalAddr(-1));
		memory->setPage(page_index, physical);

		// map the new page into the address space
		uint32_t page_flags = 0;
		if(mapping->writePermission)
			page_flags |= PageSpace::kAccessWrite;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;

		p_pageSpace.unmapSingle4k(page_vaddr);
		p_pageSpace.mapSingle4k(physical_guard, page_vaddr, physical, true, page_flags);
		if(physical_guard.isLocked())
			physical_guard.unlock();

		return true;
	}

	return false;
}

KernelSharedPtr<AddressSpace> AddressSpace::fork(Guard &guard) {
	assert(guard.protects(&lock));

	auto forked = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());

	cloneRecursive(p_root, forked.get());

	return frigg::move(forked);
}

void AddressSpace::activate() {
	p_pageSpace.activate();
}

Mapping *AddressSpace::getMapping(VirtualAddr address) {
	Mapping *current = p_root;
	
	while(current != nullptr) {
		if(address < current->baseAddress) {
			current = current->leftPtr;
		}else if(address >= current->baseAddress + current->length) {
			current = current->rightPtr;
		}else{
			assert(address >= current->baseAddress
					&& address < current->baseAddress + current->length);
			return current;
		}
	}

	return nullptr;
}

Mapping *AddressSpace::allocate(size_t length, MapFlags flags) {
	assert((length % kPageSize) == 0);

	if(p_root->largestHole < length)
		return nullptr;
	
	return allocateDfs(p_root, length, flags);
}

Mapping *AddressSpace::allocateDfs(Mapping *mapping, size_t length,
		MapFlags flags) {
	if((flags & kMapPreferBottom) != 0) {
		// try to allocate memory at the bottom of the range
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, 0, length);
		
		if(mapping->leftPtr && mapping->leftPtr->largestHole >= length)
			return allocateDfs(mapping->leftPtr, length, flags);
		
		assert(mapping->rightPtr && mapping->rightPtr->largestHole >= length);
		return allocateDfs(mapping->rightPtr, length, flags);
	}else{
		// try to allocate memory at the top of the range
		assert((flags & kMapPreferTop) != 0);
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, mapping->length - length, length);

		if(mapping->rightPtr && mapping->rightPtr->largestHole >= length)
			return allocateDfs(mapping->rightPtr, length, flags);
		
		assert(mapping->leftPtr && mapping->leftPtr->largestHole >= length);
		return allocateDfs(mapping->leftPtr, length, flags);
	}
}

Mapping *AddressSpace::allocateAt(VirtualAddr address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	Mapping *hole = getMapping(address);
	assert(hole != nullptr);
	assert(hole->type == Mapping::kTypeHole);
	
	return splitHole(hole, address - hole->baseAddress, length);
}

void AddressSpace::cloneRecursive(Mapping *mapping, AddressSpace *dest_space) {
	Mapping *dest_mapping = frigg::construct<Mapping>(*kernelAlloc, mapping->type,
			mapping->baseAddress, mapping->length);

	if(mapping->type == Mapping::kTypeHole) {
		// holes do not require additional handling
	}else if(mapping->type == Mapping::kTypeMemory
			&& (mapping->flags & Mapping::kFlagShareOnFork)) {
		KernelUnsafePtr<Memory> memory = mapping->memoryRegion;
		assert(memory->getType() == Memory::kTypeAllocated
				|| memory->getType() == Memory::kTypePhysical);

		uint32_t page_flags = 0;
		if(mapping->writePermission)
			page_flags |= PageSpace::kAccessWrite;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;

		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t i = 0; i < dest_mapping->length / kPageSize; i++) {
			PhysicalAddr physical = memory->getPage(i);
			if(physical == PhysicalAddr(-1))
				continue;
			VirtualAddr vaddr = dest_mapping->baseAddress + i * kPageSize;
			dest_space->p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		dest_mapping->memoryRegion = KernelSharedPtr<Memory>(memory);
		dest_mapping->writePermission = mapping->writePermission;
		dest_mapping->executePermission = mapping->executePermission;
	}else if(mapping->type == Mapping::kTypeMemory) {
		KernelUnsafePtr<Memory> memory = mapping->memoryRegion;
		assert(memory->getType() == Memory::kTypeAllocated);

		// don't set the write flag to enable copy-on-write
		uint32_t page_flags = 0;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;
		
		// create a copy-on-write region for the original space
		auto src_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeCopyOnWrite);
		src_memory->resize(memory->numPages());
		src_memory->master = KernelSharedPtr<Memory>(memory);
		mapping->memoryRegion = frigg::move(src_memory);
		
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t i = 0; i < mapping->length / kPageSize; i++) {
			PhysicalAddr physical = memory->getPage(i);
			if(physical == PhysicalAddr(-1))
				continue;
			VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;
			p_pageSpace.unmapSingle4k(vaddr);
			p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		// we need to release the lock before calling makeShared()
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		// create a copy-on-write region for the forked space
		auto dest_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeCopyOnWrite);
		dest_memory->resize(memory->numPages());
		dest_memory->master = KernelSharedPtr<Memory>(memory);
		dest_mapping->memoryRegion = frigg::move(dest_memory);
		
		for(size_t i = 0; i < mapping->length / kPageSize; i++) {
			PhysicalAddr physical = memory->getPage(i);
			if(physical == PhysicalAddr(-1))
				continue;
			VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;
			dest_space->p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		dest_mapping->writePermission = mapping->writePermission;
		dest_mapping->executePermission = mapping->executePermission;
	}else{
		assert(!"Illegal mapping type");
	}

	dest_space->addressTreeInsert(dest_mapping);

	if(mapping->leftPtr)
		cloneRecursive(mapping->leftPtr, dest_space);
	if(mapping->rightPtr)
		cloneRecursive(mapping->rightPtr, dest_space);
}

Mapping *AddressSpace::splitHole(Mapping *mapping,
		VirtualAddr split_offset, size_t split_length) {
	assert(split_length > 0);
	assert(mapping->type == Mapping::kTypeHole);
	assert(split_offset + split_length <= mapping->length);
	
	VirtualAddr hole_address = mapping->baseAddress;
	size_t hole_length = mapping->length;
	
	if(split_offset == 0) {
		// the split mapping starts at the beginning of the hole
		// we have to delete the hole mapping
		addressTreeRemove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
	}else{
		// the split mapping starts in the middle of the hole
		mapping->length = split_offset;
		updateLargestHoleUpwards(mapping);
	}

	auto split = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeNone,
			hole_address + split_offset, split_length);
	addressTreeInsert(split);

	if(hole_length > split_offset + split_length) {
		// the split mapping does not go on until the end of the hole
		// we have to create another mapping for the rest of the hole
		auto following = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
				hole_address + (split_offset + split_length),
				hole_length - (split_offset + split_length));
		addressTreeInsert(following);
	}else{
		assert(hole_length == split_offset + split_length);
	}

	return split;
}

void AddressSpace::rotateLeft(Mapping *n) {
	Mapping *u = n->parentPtr;
	assert(u != nullptr && u->rightPtr == n);
	Mapping *v = n->leftPtr;
	Mapping *w = u->parentPtr;

	if(v != nullptr)
		v->parentPtr = u;
	u->rightPtr = v;
	u->parentPtr = n;
	n->leftPtr = u;
	n->parentPtr = w;

	if(w == nullptr) {
		p_root = n;
	}else if(w->leftPtr == u) {
		w->leftPtr = n;
	}else{
		assert(w->rightPtr == u);
		w->rightPtr = n;
	}

	updateLargestHoleAt(u);
	updateLargestHoleAt(n);
}

void AddressSpace::rotateRight(Mapping *n) {
	Mapping *u = n->parentPtr;
	assert(u != nullptr && u->leftPtr == n);
	Mapping *v = n->rightPtr;
	Mapping *w = u->parentPtr;
	
	if(v != nullptr)
		v->parentPtr = u;
	u->leftPtr = v;
	u->parentPtr = n;
	n->rightPtr = u;
	n->parentPtr = w;

	if(w == nullptr) {
		p_root = n;
	}else if(w->leftPtr == u) {
		w->leftPtr = n;
	}else{
		assert(w->rightPtr == u);
		w->rightPtr = n;
	}

	updateLargestHoleAt(u);
	updateLargestHoleAt(n);
}

bool AddressSpace::isRed(Mapping *mapping) {
	if(mapping == nullptr)
		return false;
	return mapping->color == Mapping::kColorRed;
}
bool AddressSpace::isBlack(Mapping *mapping) {
	if(mapping == nullptr)
		return true;
	return mapping->color == Mapping::kColorBlack;
}

void AddressSpace::addressTreeInsert(Mapping *mapping) {
	assert(checkInvariant());

	if(!p_root) {
		p_root = mapping;

		fixAfterInsert(mapping);
		assert(checkInvariant());
		return;
	}

	Mapping *current = p_root;
	while(true) {
		if(mapping->baseAddress < current->baseAddress) {
			assert(mapping->baseAddress + mapping->length <= current->baseAddress);
			if(current->leftPtr == nullptr) {
				current->leftPtr = mapping;
				mapping->parentPtr = current;

				// "current" is the successor of "mapping"
				Mapping *predecessor = current->lowerPtr;
				if(predecessor)
					predecessor->higherPtr = mapping;
				mapping->lowerPtr = predecessor;
				mapping->higherPtr = current;
				current->lowerPtr = mapping;

				updateLargestHoleUpwards(current);

				fixAfterInsert(mapping);
				assert(checkInvariant());
				return;
			}else{
				current = current->leftPtr;
			}
		}else{
			assert(mapping->baseAddress >= current->baseAddress + current->length);
			if(current->rightPtr == nullptr) {
				current->rightPtr = mapping;
				mapping->parentPtr = current;

				// "current" is the predecessor of "mapping"
				Mapping *successor = current->higherPtr;
				current->higherPtr = mapping;
				mapping->lowerPtr = current;
				mapping->higherPtr = successor;
				if(successor)
					successor->lowerPtr = mapping;
				
				updateLargestHoleUpwards(current);
				
				fixAfterInsert(mapping);
				assert(checkInvariant());
				return;
			}else{
				current = current->rightPtr;
			}
		}
	}
}

// Situation:
// |     (p)     |
// |    /   \    |
// |  (s)   (n)  |
// Precondition: The red-black property is only violated in the following sense:
//     Paths from (p) over (n) to a leaf contain one black node more
//     than paths from (p) over (s) to a leaf
// Postcondition: The whole tree is a red-black tree
void AddressSpace::fixAfterInsert(Mapping *n) {
	Mapping *parent = n->parentPtr;
	if(parent == nullptr) {
		n->color = Mapping::kColorBlack;
		return;
	}
	
	n->color = Mapping::kColorRed;

	if(parent->color == Mapping::kColorBlack)
		return;
	
	// the rb invariants guarantee that a grandparent exists
	Mapping *grand = parent->parentPtr;
	assert(grand && grand->color == Mapping::kColorBlack);
	
	// handle the red uncle case
	if(grand->leftPtr == parent && isRed(grand->rightPtr)) {
		grand->color = Mapping::kColorRed;
		parent->color = Mapping::kColorBlack;
		grand->rightPtr->color = Mapping::kColorBlack;

		fixAfterInsert(grand);
		return;
	}else if(grand->rightPtr == parent && isRed(grand->leftPtr)) {
		grand->color = Mapping::kColorRed;
		parent->color = Mapping::kColorBlack;
		grand->leftPtr->color = Mapping::kColorBlack;

		fixAfterInsert(grand);
		return;
	}
	
	if(parent == grand->leftPtr) {
		if(n == parent->rightPtr) {
			rotateLeft(n);
			rotateRight(n);
			n->color = Mapping::kColorBlack;
		}else{
			rotateRight(parent);
			parent->color = Mapping::kColorBlack;
		}
		grand->color = Mapping::kColorRed;
	}else{
		assert(parent == grand->rightPtr);
		if(n == parent->leftPtr) {
			rotateRight(n);
			rotateLeft(n);
			n->color = Mapping::kColorBlack;
		}else{
			rotateLeft(parent);
			parent->color = Mapping::kColorBlack;
		}
		grand->color = Mapping::kColorRed;
	}
}

void AddressSpace::addressTreeRemove(Mapping *mapping) {
	assert(checkInvariant());

	Mapping *left_ptr = mapping->leftPtr;
	Mapping *right_ptr = mapping->rightPtr;

	if(!left_ptr) {
		removeHalfLeaf(mapping, right_ptr);
	}else if(!right_ptr) {
		removeHalfLeaf(mapping, left_ptr);
	}else{
		// replace the mapping by its predecessor
		Mapping *predecessor = mapping->lowerPtr;
		removeHalfLeaf(predecessor, predecessor->leftPtr);
		replaceNode(mapping, predecessor);
	}
	
	assert(checkInvariant());
}

void AddressSpace::replaceNode(Mapping *node, Mapping *replacement) {
	Mapping *parent = node->parentPtr;
	Mapping *left = node->leftPtr;
	Mapping *right = node->rightPtr;

	// fix the red-black tree
	if(parent == nullptr) {
		p_root = replacement;
	}else if(node == parent->leftPtr) {
		parent->leftPtr = replacement;
	}else{
		assert(node == parent->rightPtr);
		parent->rightPtr = replacement;
	}
	replacement->parentPtr = parent;
	replacement->color = node->color;

	replacement->leftPtr = left;
	if(left)
		left->parentPtr = replacement;
	
	replacement->rightPtr = right;
	if(right)
		right->parentPtr = replacement;
	
	// fix the linked list
	if(node->lowerPtr)
		node->lowerPtr->higherPtr = replacement;
	replacement->lowerPtr = node->lowerPtr;
	replacement->higherPtr = node->higherPtr;
	if(node->higherPtr)
		node->higherPtr->lowerPtr = replacement;
	
	node->leftPtr = nullptr;
	node->rightPtr = nullptr;
	node->parentPtr = nullptr;
	node->lowerPtr = nullptr;
	node->higherPtr = nullptr;
	
	updateLargestHoleAt(replacement);
	updateLargestHoleUpwards(parent);
}

void AddressSpace::removeHalfLeaf(Mapping *mapping, Mapping *child) {
	Mapping *predecessor = mapping->lowerPtr;
	Mapping *successor = mapping->higherPtr;
	if(predecessor)
		predecessor->higherPtr = successor;
	if(successor)
		successor->lowerPtr = predecessor;

	if(mapping->color == Mapping::kColorBlack) {
		if(isRed(child)) {
			child->color = Mapping::kColorBlack;
		}else{
			// decrement the number of black nodes all paths through "mapping"
			// before removing the child. this makes sure we're correct even when
			// "child" is null
			fixAfterRemove(mapping);
		}
	}
	
	assert((!mapping->leftPtr && mapping->rightPtr == child)
			|| (mapping->leftPtr == child && !mapping->rightPtr));
		
	Mapping *parent = mapping->parentPtr;
	if(!parent) {
		p_root = child;
	}else if(parent->leftPtr == mapping) {
		parent->leftPtr = child;
	}else{
		assert(parent->rightPtr == mapping);
		parent->rightPtr = child;
	}
	if(child)
		child->parentPtr = parent;
	
	mapping->leftPtr = nullptr;
	mapping->rightPtr = nullptr;
	mapping->parentPtr = nullptr;
	mapping->lowerPtr = nullptr;
	mapping->higherPtr = nullptr;
	
	if(parent)
		updateLargestHoleUpwards(parent);
}

// Situation:
// |     (p)     |
// |    /   \    |
// |  (s)   (n)  |
// Precondition: The red-black property is only violated in the following sense:
//     Paths from (p) over (n) to a leaf contain one black node less
//     than paths from (p) over (s) to a leaf
// Postcondition: The whole tree is a red-black tree
void AddressSpace::fixAfterRemove(Mapping *n) {
	assert(n->color == Mapping::kColorBlack);
	
	Mapping *parent = n->parentPtr;
	if(parent == nullptr)
		return;
	
	// rotate so that our node has a black sibling
	Mapping *s; // this will always be the sibling of our node
	if(parent->leftPtr == n) {
		assert(parent->rightPtr);
		if(parent->rightPtr->color == Mapping::kColorRed) {
			Mapping *x = parent->rightPtr;
			rotateLeft(parent->rightPtr);
			assert(n == parent->leftPtr);
			
			parent->color = Mapping::kColorRed;
			x->color = Mapping::kColorBlack;
		}
		
		s = parent->rightPtr;
	}else{
		assert(parent->rightPtr == n);
		assert(parent->leftPtr);
		if(parent->leftPtr->color == Mapping::kColorRed) {
			Mapping *x = parent->leftPtr;
			rotateRight(x);
			assert(n == parent->rightPtr);
			
			parent->color = Mapping::kColorRed;
			x->color = Mapping::kColorBlack;
		}
		
		s = parent->leftPtr;
	}
	
	if(isBlack(s->leftPtr) && isBlack(s->rightPtr)) {
		if(parent->color == Mapping::kColorBlack) {
			s->color = Mapping::kColorRed;
			fixAfterRemove(parent);
			return;
		}else{
			parent->color = Mapping::kColorBlack;
			s->color = Mapping::kColorRed;
			return;
		}
	}
	
	// now at least one of s children is red
	Mapping::Color parent_color = parent->color;
	if(parent->leftPtr == n) {
		// rotate so that s->rightPtr is red
		if(isRed(s->leftPtr) && isBlack(s->rightPtr)) {
			Mapping *child = s->leftPtr;
			rotateRight(child);

			s->color = Mapping::kColorRed;
			child->color = Mapping::kColorBlack;

			s = child;
		}
		assert(isRed(s->rightPtr));

		rotateLeft(s);
		parent->color = Mapping::kColorBlack;
		s->color = parent_color;
		s->rightPtr->color = Mapping::kColorBlack;
	}else{
		assert(parent->rightPtr == n);

		// rotate so that s->leftPtr is red
		if(isRed(s->rightPtr) && isBlack(s->leftPtr)) {
			Mapping *child = s->rightPtr;
			rotateLeft(child);

			s->color = Mapping::kColorRed;
			child->color = Mapping::kColorBlack;

			s = child;
		}
		assert(isRed(s->leftPtr));

		rotateRight(s);
		parent->color = Mapping::kColorBlack;
		s->color = parent_color;
		s->leftPtr->color = Mapping::kColorBlack;
	}
}

bool AddressSpace::checkInvariant() {
	if(!p_root)
		return true;

	int black_depth;
	Mapping *minimal, *maximal;
	return checkInvariant(p_root, black_depth, minimal, maximal);
}

bool AddressSpace::checkInvariant(Mapping *mapping, int &black_depth,
		Mapping *&minimal, Mapping *&maximal) {
	// check largest hole invariant
	size_t hole = 0;
	if(mapping->type == Mapping::kTypeHole)
		hole = mapping->length;
	if(mapping->leftPtr && mapping->leftPtr->largestHole > hole)
		hole = mapping->leftPtr->largestHole;
	if(mapping->rightPtr && mapping->rightPtr->largestHole > hole)
		hole = mapping->rightPtr->largestHole;
	
	if(mapping->largestHole != hole) {
		infoLogger->log() << "largestHole violation" << frigg::EndLog();
		return false;
	}

	// check alternating colors invariant
	if(mapping->color == Mapping::kColorRed)
		if(!isBlack(mapping->leftPtr) || !isBlack(mapping->rightPtr)) {
			infoLogger->log() << "Alternating colors violation" << frigg::EndLog();
			return false;
		}
	
	// check recursive invariants
	int left_black_depth = 0;
	int right_black_depth = 0;
	
	if(mapping->leftPtr) {
		Mapping *predecessor;
		if(!checkInvariant(mapping->leftPtr, left_black_depth, minimal, predecessor))
			return false;

		// check search tree invariant
		if(mapping->baseAddress < predecessor->baseAddress + predecessor->length) {
			infoLogger->log() << "Search tree (left) violation" << frigg::EndLog();
			return false;
		}
		
		// check predecessor invariant
		if(predecessor->higherPtr != mapping) {
			infoLogger->log() << "Linked list (predecessor, forward) violation" << frigg::EndLog();
			return false;
		}else if(mapping->lowerPtr != predecessor) {
			infoLogger->log() << "Linked list (predecessor, backward) violation" << frigg::EndLog();
			return false;
		}
	}else{
		minimal = mapping;
	}

	if(mapping->rightPtr) {
		Mapping *successor;
		if(!checkInvariant(mapping->rightPtr, right_black_depth, successor, maximal))
			return false;
		
		// check search tree invariant
		if(mapping->baseAddress + mapping->length > successor->baseAddress) {
			infoLogger->log() << "Search tree (right) violation" << frigg::EndLog();
			return false;
		}

		// check successor invariant
		if(mapping->higherPtr != successor) {
			infoLogger->log() << "Linked list (successor, forward) violation" << frigg::EndLog();
			return false;
		}else if(successor->lowerPtr != mapping) {
			infoLogger->log() << "Linked list (successor, backward) violation" << frigg::EndLog();
			return false;
		}
	}else{
		maximal = mapping;
	}
	
	// check black-depth invariant
	if(left_black_depth != right_black_depth) {
		infoLogger->log() << "Black-depth violation" << frigg::EndLog();
		return false;
	}

	black_depth = left_black_depth;
	if(mapping->color == Mapping::kColorBlack)
		black_depth++;
	
	return true;
}

bool AddressSpace::updateLargestHoleAt(Mapping *mapping) {
	size_t hole = 0;
	if(mapping->type == Mapping::kTypeHole)
		hole = mapping->length;
	if(mapping->leftPtr && mapping->leftPtr->largestHole > hole)
		hole = mapping->leftPtr->largestHole;
	if(mapping->rightPtr && mapping->rightPtr->largestHole > hole)
		hole = mapping->rightPtr->largestHole;
	
	if(mapping->largestHole != hole) {
		mapping->largestHole = hole;
		return true;
	}
	return false;
}
void AddressSpace::updateLargestHoleUpwards(Mapping *mapping) {
	if(!updateLargestHoleAt(mapping))
		return;
	
	if(mapping->parentPtr != nullptr)
		updateLargestHoleUpwards(mapping->parentPtr);
}

} // namespace thor

