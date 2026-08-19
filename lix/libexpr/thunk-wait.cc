#include "lix/libexpr/thunk-wait.hh"
#include "lix/libutil/signals.hh"
#include "lix/libutil/sync.hh"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>

namespace nix {

// Cache line alignment to prevent false sharing.
struct alignas(64) WaiterDomain
{
    std::condition_variable cv;
};

static std::array<Sync<WaiterDomain>, 128> waiterDomains;

static Sync<WaiterDomain> & getWaiterDomain(Value::Thunk & thunk)
{
    return waiterDomains[(((size_t) &thunk) >> 5) % waiterDomains.size()];
}

static std::atomic<uint32_t> nextEvalThreadId{1};

[[gnu::tls_model("initial-exec")]]
static thread_local uint32_t myEvalThreadId_ = 0;

uint32_t getMyEvalThreadId()
{
    if (myEvalThreadId_ == 0) {
        myEvalThreadId_ = nextEvalThreadId.fetch_add(1, std::memory_order_relaxed);
    }
    return myEvalThreadId_;
}

void waitOnThunk(Value::Thunk & thunk)
{
    auto domain = getWaiterDomain(thunk).lock();
    while ((thunk.state.load(std::memory_order_acquire) & ThunkStateMask)
           == static_cast<uint32_t>(ThunkState::Awaited))
    {
        checkInterrupt();
        domain.wait(domain->cv);
    }
}

void notifyThunkWaiters(Value::Thunk & thunk)
{
    auto domain = getWaiterDomain(thunk).lock();
    domain->cv.notify_all();
}

void wakeAllThunkWaiters()
{
    for (auto & domain : waiterDomains) {
        domain.lock()->cv.notify_all();
    }
}

} // namespace nix
