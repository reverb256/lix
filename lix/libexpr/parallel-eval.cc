#include "lix/libexpr/parallel-eval.hh"
#include "lix/libexpr/eval-inline.hh" // IWYU pragma: keep
#include "lix/libexpr/eval-settings.hh"
#include "lix/libexpr/nixexpr.hh"
#include "lix/libexpr/primops.hh"
#include "lix/libexpr/thunk-wait.hh"
#include "lix/libstore/globals.hh"
#include "lix/libutil/async.hh"
#include "lix/libutil/logging.hh"
#include "lix/libutil/signals.hh"

#include <kj/exception.h>

#include <algorithm>
#include <array>
#include <exception>
#include <string_view>
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

    // Give this worker its own kj event loop. The evaluator's store
    // interactions (writeDerivation/addToStore/readDerivation, ...) are
    // async (kj::Promise) and their internal locks are kj async mutexes
    // (Sync<T, AsyncMutex>), so any of them requires an event loop on the
    // calling thread. Without this, `AIO()`/`co_await` on a worker throws
    // "No event loop is running on this thread" and the work item fails.
    // This mirrors Lix's own ThreadPool::doWork(), which gives every
    // worker its own AsyncIoRoot; AsyncIoRoot::blockOn() is thread-agnostic
    // (it waits on the thread-local kj::waitScope), so the existing
    // state.aio.blockOn(...) call sites work unchanged.
    AsyncIoRoot aio;

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
        } catch (const kj::Exception & e) {
            // kj::Exception objects are thread-affine: ExceptionImpl
            // registers itself in a thread-local in-flight list and its
            // destructor aborts if it is destroyed on a different thread
            // than the one that threw it. Never let one cross the worker
            // boundary; convert it to a nix::Error (a plain std::exception)
            // so the main thread can safely rethrow it.
            item.promise.set_exception(std::make_exception_ptr(Error(e.getDescription().cStr())));
        } catch (const ForeignException & e) {
            // A rejected async operation (e.g. a store call) surfaces as a
            // kj::Exception, which AsyncIoRoot::blockOn() then wraps in a
            // ForeignException (its `inner` is an exception_ptr to the
            // thread-affine kj::ExceptionImpl). Unwrap it and convert to a
            // plain Error so the kj::Exception is released on this worker
            // thread instead of on the main thread.
            if (auto * kje = e.as<kj::Exception>()) {
                item.promise.set_exception(std::make_exception_ptr(Error(kje->getDescription().cStr())));
            } else {
                item.promise.set_exception(std::current_exception());
            }
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

/**
 * Whether forcing a value (an unresolved thunk or a pending function
 * application) is likely to perform store/async I/O (path copying,
 * derivation writing/realisation, fetching), which blocks and therefore
 * overlaps well across worker threads. Pure computation is deliberately
 * excluded so it stays on the walking thread and does not contend on the
 * Boehm GC's global allocation lock (running many allocation-heavy thunks
 * concurrently is *slower* than serially).
 *
 * This is a shallow, conservative syntactic test: it recognises direct
 * store builtins and `drvPath`/`outPath` selects, following lambda bodies
 * so pending applications like `builtins.map (d: d.drvPath) ...` are
 * caught. Deeper wrappers (e.g. `stdenv.mkDerivation`) are forced inline,
 * and their store work still happens nested inside a recognised
 * `drvPath`/`outPath` work item.
 */

/** Store/async-bound builtins, by their user-visible name (after the
 * `__` registration prefix is stripped). */
static bool isStoreBoundBuiltin(std::string_view name)
{
    static const std::array<std::string_view, 17> names = {
        "derivation",
        "derivationStrict",
        "fetchGit",
        "fetchMercurial",
        "fetchTarball",
        "fetchTree",
        "fetchurl",
        "filterSource",
        "import",
        "scopedImport",
        "path",
        "pathExists",
        "readDir",
        "readFile",
        "storePath",
        "toFile",
        "toPath",
    };
    return std::find(names.begin(), names.end(), name) != names.end();
}

static bool isStoreBoundExpr(EvalState & state, Expr & e)
{
    // A lambda's store work happens in its body when it is applied.
    if (auto * lambda = e.try_cast<ExprLambda>()) {
        return isStoreBoundExpr(state, *lambda->body);
    }

    // A call to a store-bound builtin, either bare (`derivation { ... }`,
    // `import ./x.nix`) or via the `builtins` attrset
    // (`builtins.fetchTarball { ... }`).
    if (auto * call = e.try_cast<ExprCall>()) {
        Expr & fun = *call->fun;
        if (auto * var = fun.try_cast<ExprVar>()) {
            if (isStoreBoundBuiltin(state.ctx.symbols[var->name])) {
                return true;
            }
        } else if (auto * sel = fun.try_cast<ExprSelect>()) {
            if (sel->attrPath.size() == 1 && !sel->attrPath[0].isDynamic()) {
                if (auto * base = sel->e->try_cast<ExprVar>()) {
                    if (state.ctx.symbols[base->name] == "builtins"
                        && isStoreBoundBuiltin(state.ctx.symbols[sel->attrPath[0].symbol]))
                    {
                        return true;
                    }
                }
            }
        }
    }

    // Selecting `drvPath`/`outPath` writes or realises a derivation.
    if (auto * sel = e.try_cast<ExprSelect>()) {
        if (sel->attrPath.size() == 1 && !sel->attrPath[0].isDynamic()) {
            std::string_view name = state.ctx.symbols[sel->attrPath[0].symbol];
            if (name == "drvPath" || name == "outPath") {
                return true;
            }
        }
    }

    return false;
}

static bool isStoreBoundValue(EvalState & state, const Value & v)
{
    // A real thunk (internalType tThunk) has an `expr` to inspect. Note
    // `type()` reports nThunk for both unresolved thunks and pending
    // function applications (App values), so it cannot be used here.
    if (v.isThunk() && !v.thunk().resolved()) {
        Expr * e = v.thunk().expr;
        return e != nullptr && isStoreBoundExpr(state, *e);
    }
    // A pending function application (e.g. every element of a
    // `builtins.map (d: d.drvPath) ...` result is an App): inspect the
    // applied lambda's body for store work.
    if (v.isApp()) {
        const Value & target = v.app().target();
        if (target.isLambda()) {
            if (auto * lambda = target.lambda().fun) {
                return isStoreBoundExpr(state, *lambda->body);
            }
        }
    }
    return false;
}

/**
 * Schedule forcing `child`: spawn a worker item when it is a thunk likely
 * to block on store/async I/O (so the block overlaps other work), otherwise
 * force it inline on the current thread. Values already in normal form
 * (ints, strings, paths, lambdas, ...) are skipped.
 */
static void dispatchChild(EvalState & state, Value & child, PosIdx pos, Executor::WorkItems & work)
{
    switch (child.type()) {
    case nThunk:
        // `type()` reports nThunk for both unresolved thunks and pending
        // function applications (App values); isStoreBoundValue() tells
        // them apart and classifies both.
        if (isStoreBoundValue(state, child)) {
            // allocRootValue makes a GC-rooted copy of the Value so the
            // graph stays alive for the whole duration of the work item
            // even if the top-level value goes out of scope (the executor
            // is not waited on explicitly).
            work.emplace_back(
                [value = allocRootValue(child), pos, &state]() { parallelForceDeep(state, *value, pos); }, 0
            );
        } else {
            parallelForceDeep(state, child, pos);
        }
        break;
    case nAttrs:
    case nList:
        // Already forced to normal form; walking its children is cheap and
        // discovers store-bound descendants to spawn, so do it inline.
        parallelForceDeep(state, child, pos);
        break;
    case nInt:
    case nFloat:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nExternal:
    case nFunction:
        // Already in normal form: nothing left to force.
        break;
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
            dispatchChild(state, a.value, a.pos, work);
        }
        break;
    }

    case nList: {
        for (auto & elem : v.listItems()) {
            dispatchChild(state, elem, noPos, work);
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
