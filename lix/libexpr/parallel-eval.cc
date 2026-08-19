#include "lix/libexpr/parallel-eval.hh"
#include "lix/libexpr/eval-inline.hh" // IWYU pragma: keep
#include "lix/libexpr/eval-settings.hh"
#include "lix/libexpr/primops.hh"
#include "lix/libexpr/thunk-wait.hh"
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
        // Wake any threads blocked waiting on a thunk so they can
        // observe the interrupt and unwind.
        wakeAllThunkWaiters();
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

void parallelForceDeep(EvalState & state, Value & v, PosIdx pos)
{
    state.forceValue(v, pos);

    Executor::WorkItems work;

    switch (v.type()) {

    case nAttrs: {
        NixStringContext context;
        if (state.tryAttrsToString(pos, v, context, StringCoercionMode::Strict, false)) {
            return;
        }
        if (v.attrs()->get(state.ctx.symbols.sym_outPath)) {
            return;
        }
        for (auto & a : *v.attrs()) {
            // allocRootValue makes a GC-rooted copy of the Value so
            // the graph stays alive for the whole duration of the
            // work item even if the top-level value goes out of
            // scope (the executor is not waited on explicitly).
            work.emplace_back(
                [value = allocRootValue(a.value), pos = a.pos, &state]() {
                    parallelForceDeep(state, *value, pos);
                },
                0
            );
        }
        break;
    }

    case nList: {
        for (auto & elem : v.listItems()) {
            work.emplace_back(
                [value = allocRootValue(elem), &state]() { parallelForceDeep(state, *value, noPos); }, 0
            );
        }
        break;
    }

    case nThunk:
    case nInt:
    case nFloat:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nExternal:
    case nFunction:
        break;
    }

    // Track the futures so they are drained when the EvalState is
    // destroyed; discarding them would let background work outlive the
    // state it references.
    state.getFutures().spawn(std::move(work));
}

Value prim_parallel(EvalState & state, Value ** args)
{
    state.forceList(*args[0], noPos, "while evaluating the first argument passed to builtins.parallel");

    if (state.ctx.parallelEvalEnabled()) {
        Executor::WorkItems work;
        for (auto & elem : args[0]->listItems()) {
            // Only spawn work for elements that aren't already forced;
            // the executor then evaluates them in the background while
            // we force the second argument below. Forcing `x` will block
            // on any in-flight element via the thunk waiter machinery.
            if (elem.isThunk() && !elem.thunk().resolved()) {
                // GC-rooted copy so the work item keeps the element
                // alive even if the caller drops the list before the
                // background force runs.
                work.emplace_back(
                    [value = allocRootValue(elem), &state]() { state.forceValue(*value, noPos); }, 0
                );
            }
        }
        if (!work.empty()) {
            state.getFutures().spawn(std::move(work));
        }
    }

    state.forceValue(*args[1], noPos);
    return *args[1];
}

} // namespace nix
