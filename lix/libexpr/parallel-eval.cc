#include "lix/libexpr/parallel-eval.hh"
#include "lix/libexpr/eval-settings.hh"
#include "lix/libstore/globals.hh"
#include "lix/libutil/logging.hh"
#include "lix/libutil/signals.hh"

#include <algorithm>
#include <exception>
#include <utility>

namespace nix {

[[gnu::tls_model("initial-exec")]]
thread_local bool Executor::amWorkerThread{false};

unsigned int Executor::getEvalCores(const EvalSettings & evalSettings)
{
    /* Note: the default number of cores is currently limited to 32
       due to scalability bottlenecks. */
    return evalSettings.evalCores == 0UL ? std::min(32U, settings.getDefaultCores()) : evalSettings.evalCores;
}

Executor::Executor(const EvalSettings & evalSettings)
    : evalCores(getEvalCores(evalSettings))
    , enabled(evalCores > 1)
    , interruptCallback(createInterruptCallback([&]() {
        quit = true;
        wakeup.notify_all();
    }))
{
#if HAVE_BOEHMGC
    // Allow worker threads to be registered with Boehm GC (it is not
    // implicitly called by the GC on this platform).
    GC_allow_register_threads();
#endif
    debug("executor using %1% threads", evalCores);
    auto state(state_.lock());
    for (size_t n = 0; n < evalCores; ++n) {
        try {
            createWorker(*state);
        } catch (std::system_error & e) {
            if (n == 0) {
                throw Error("could not create any evaluator worker threads: %1%", e.what());
            }
            printWarning("could only create %1% evaluator worker threads: %2%", n, e.what());
            break;
        }
    }
}

Executor::~Executor()
{
    std::vector<pthread_t> threads;
    {
        auto state(state_.lock());
        quit = true;
        std::swap(threads, state->threads);
        debug("executor shutting down with %1% items left", state->queue.size());
    }

    wakeup.notify_all();

    for (auto & thr : threads) {
        pthread_join(thr, nullptr);
    }
}

void Executor::createWorker(State & state)
{
    pthread_attr_t attrs;
    if (pthread_attr_init(&attrs) != 0) {
        throw Error("could not initialize worker thread attributes");
    }

    if (pthread_attr_setstacksize(&attrs, evalStackSize) != 0) {
        pthread_attr_destroy(&attrs);
        throw Error("could not set worker thread stack size");
    }

    pthread_t thread;
    auto ret = pthread_create(&thread, &attrs, &Executor::workerEntry, this);
    pthread_attr_destroy(&attrs);
    if (ret != 0) {
        throw std::system_error(ret, std::generic_category(), "could not create evaluator worker thread");
    }

    state.threads.push_back(thread);
}

void * Executor::workerEntry(void * arg)
{
    // Note: no explicit Boehm GC thread registration here. When GC_THREADS
    // is defined (see parallel-eval.hh), gc.h redirects pthread_create to
    // GC_pthread_create, which wraps the entry point in
    // GC_call_with_stack_base() and registers/unregisters the thread with the
    // collector automatically. Registering again here would corrupt GC state
    // (double registration).
    auto * self = static_cast<Executor *>(arg);
    self->worker();
    return nullptr;
}

void Executor::worker()
{
    ReceiveInterrupts receiveInterrupts;

    interruptCheck = [&]() { return (bool) quit; };

    amWorkerThread = true;

    while (true) {
        Item item;

        while (true) {
            auto state(state_.lock());
            if (quit) {
                // Set an `Interrupted` exception on all promises so
                // we get a nicer error than "std::future_error:
                // Broken promise".
                auto ex = std::make_exception_ptr(makeInterrupted());
                for (auto & item : state->queue) {
                    item.second.promise.set_exception(ex);
                }
                state->queue.clear();
                return;
            }
            if (!state->queue.empty()) {
                item = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                break;
            }
            state.wait(wakeup);
        }

        try {
            item.work();
            item.promise.set_value();
        } catch (const Interrupted &) {
            quit = true;
            item.promise.set_exception(std::current_exception());
        } catch (...) {
            item.promise.set_exception(std::current_exception());
        }
    }
}

std::vector<std::future<void>> Executor::spawn(WorkItems && items)
{
    if (items.empty()) {
        return {};
    }

    std::vector<std::future<void>> futures;

    {
        auto state(state_.lock());
        for (auto & item : items) {
            std::promise<void> promise;
            futures.push_back(promise.get_future());
            /* Note: this uses a cheap PRNG rather than std::random_device,
               since the latter costs hundreds of cycles per call (RDRAND or
               /dev/urandom), which adds up when spawning many work items. The
               key only needs to spread items of the same priority around the
               queue, not be cryptographically random. */
            [[gnu::tls_model("initial-exec")]]
            static thread_local std::mt19937_64 rng{std::random_device{}()};
            [[gnu::tls_model("initial-exec")]]
            static thread_local std::uniform_int_distribution<uint64_t> dist(0, 1ULL << 48);
            auto key = (uint64_t(item.second) << 48) | dist(rng);
            state->queue.emplace(key, Item{.promise = std::move(promise), .work = std::move(item.first)});
        }
    }

    if (items.size() == 1) {
        wakeup.notify_one();
    } else {
        wakeup.notify_all();
    }

    return futures;
}

FutureVector::~FutureVector()
{
    try {
        finishAll();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void FutureVector::spawn(Executor::WorkItems && work)
{
    auto futures = executor.spawn(std::move(work));
    auto state(state_.lock());
    for (auto & future : futures) {
        state->futures.push_back(std::move(future));
    }
}

void FutureVector::finishAll()
{
    std::exception_ptr ex;
    while (true) {
        std::vector<std::future<void>> futures;
        {
            auto state(state_.lock());
            std::swap(futures, state->futures);
        }
        debug("got %1% futures", futures.size());
        if (futures.empty()) {
            break;
        }
        for (auto & future : futures) {
            try {
                future.get();
            } catch (...) {
                if (!ex) {
                    ex = std::current_exception();
                }
            }
        }
    }
    if (ex) {
        std::rethrow_exception(ex);
    }
}

} // namespace nix
