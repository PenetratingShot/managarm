
namespace thor {

int64_t allocAsyncId();

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

class BochsSink {
public:
	void print(char c);
	void print(const char *str);
};

extern BochsSink infoSink;
extern frigg::util::LazyInitializer<frigg::debug::DefaultLogger<BochsSink>> infoLogger;

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

class KernelVirtualAlloc {
public:
	KernelVirtualAlloc();

	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);

private:
	uintptr_t p_nextPage;
};

typedef frigg::memory::DebugAllocator<KernelVirtualAlloc,
		frigg::TicketLock> KernelAlloc;
extern frigg::util::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
extern frigg::util::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
extern frigg::util::LazyInitializer<KernelAlloc> kernelAlloc;

template<typename T>
using KernelSharedPtr = frigg::SharedPtr<T, KernelAlloc>;

template<typename T>
using KernelWeakPtr = frigg::WeakPtr<T, KernelAlloc>;

template<typename T>
using KernelUnsafePtr = frigg::UnsafePtr<T, KernelAlloc>;

// --------------------------------------------------------
// Kernel data types
// --------------------------------------------------------

enum Error {
	kErrSuccess,
	kErrBufferTooSmall
};

typedef uint64_t Handle;

class Universe;
class Memory;
class AddressSpace;
class Thread;
class EventHub;
class Channel;
class BiDirectionPipe;
class Server;
class RdFolder;
class IrqLine;
class IoSpace;

struct CpuContext {
	KernelUnsafePtr<Thread> currentThread;
};

struct Timer {
	Timer(uint64_t deadline)
	: deadline(deadline) { }

	bool operator< (const Timer &other) {
		return deadline < other.deadline;
	}

	uint64_t deadline;

	KernelWeakPtr<Thread> thread;
};

} // namespace thor

#include "descriptor.hpp"
#include "usermem.hpp"
#include "thread.hpp"
#include "event.hpp"
#include "ipc.hpp"
#include "rd.hpp"
#include "io.hpp"

namespace thor {

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

class Universe {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<frigg::TicketLock> Guard;

	Universe();
	
	Handle attachDescriptor(Guard &guard, AnyDescriptor &&descriptor);

	AnyDescriptor &getDescriptor(Guard &guard, Handle handle);
	
	AnyDescriptor detachDescriptor(Guard &guard, Handle handle);

	Lock lock;

private:
	frigg::util::Hashmap<Handle, AnyDescriptor,
			frigg::util::DefaultHasher<Handle>, KernelAlloc> p_descriptorMap;
	Handle p_nextHandle;
};

} // namespace thor
