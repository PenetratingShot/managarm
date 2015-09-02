
namespace thor {

struct SubmitInfo {
	SubmitInfo(int64_t async_id, uintptr_t submit_function,
			uintptr_t submit_object);
	
	int64_t asyncId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
};

struct UserEvent {
	enum Type {
		kTypeNone,
		kTypeRecvStringTransfer,
		kTypeRecvStringError,
		kTypeRecvDescriptor,
		kTypeAccept,
		kTypeConnect,
		kTypeIrq
	};

	UserEvent(Type type, SubmitInfo submit_info);
	
	Type type;
	SubmitInfo submitInfo;

	// used by kTypeRecvStringError
	Error error;
	
	// used by kTypeRecvStringTransfer and kTypeRecvDescriptor
	int64_t msgRequest;
	int64_t msgSequence;

	// used by kTypeRecvStringTransfer
	uint8_t *kernelBuffer;
	uint8_t *userBuffer;
	size_t length;

	// used by kTypeAccept, kTypeConnect
	KernelSharedPtr<BiDirectionPipe> pipe;

	// used by kTypeRecvDescriptor
	AnyDescriptor descriptor;
};

class EventHub {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	EventHub();

	void raiseEvent(Guard &guard, UserEvent &&event);

	bool hasEvent(Guard &guard);

	UserEvent dequeueEvent(Guard &guard);

	void blockCurrentThread(Guard &guard);

	Lock lock;

private:
	frigg::util::LinkedList<UserEvent, KernelAlloc> p_queue;
	frigg::util::LinkedList<KernelWeakPtr<Thread>, KernelAlloc> p_waitingThreads;
};

} // namespace thor
