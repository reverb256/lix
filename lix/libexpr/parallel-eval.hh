#pragma once
///@file
///
/// Parallel evaluation executor, ported from Determinate Nix
/// (DeterminateSystems/nix-src, src/libexpr/parallel-eval.{hh,cc}).
///
/// Provides a thread pool ("executor") that evaluates Nix values in
/// parallel, plus the `eval-cores` setting that controls it.
///
/// Stage 1: infrastructure only — the evaluator is not wired up yet
/// (see .plans/2026-08-18-parallel-eval-design.md).

#if HAVE_BOEHMGC
// Boehm GC only declares the thread-registration API (GC_register_my_thread,
// GC_unregister_my_thread) when GC_THREADS is defined at gc.h include time.
// It must be defined before any gc.h include in this TU. This matches
// Determinate Nix's eval-gc.hh; the allocation entry points (GC_malloc etc.)
// are unaffected, so it is safe to mix with TUs that do not define it.
#define GC_THREADS 1
#endif

#include "lix/libexpr/eval-settings.hh"
#include "lix/libexpr/pos-idx.hh"
#include "lix/libutil/signals.hh"
#include "lix/libutil/sync.hh"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <pthread.h>
#include <random>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if HAVE_BOEHMGC
#include <gc/gc.h>
#endif

namespace nix {

class EvalState;
struct Value;

/**
 * Move-only callable, for executor work items.
 *
 * C++23 ships std::move_only_function, but it isn't available on all
 * stdlibs (see the note in signals.hh), so use the classic pimpl
 * pattern instead.
 */
template<typename Signature>
class MoveOnlyFunction;

template<typename R, typename... Args>
class MoveOnlyFunction<R(Args...)>
{
    struct CallableBase
    {
        virtual ~CallableBase() = default;
        virtual R invoke(Args &&... args) = 0;
    };

    template<typename F>
    struct Callable : CallableBase
    {
        F f;

        explicit Callable(F && f) : f(std::move(f)) {}

        R invoke(Args &&... args) override
        {
            return std::invoke(f, std::forward<Args>(args)...);
        }
    };

    std::unique_ptr<CallableBase> impl;

public:
    MoveOnlyFunction() = default;

    template<typename F>
        requires(!std::is_same_v<std::decay_t<F>, MoveOnlyFunction>)
    MoveOnlyFunction(F && f) : impl(std::make_unique<Callable<std::decay_t<F>>>(std::forward<F>(f)))
    {
    }

    MoveOnlyFunction(MoveOnlyFunction &&) = default;
    MoveOnlyFunction & operator=(MoveOnlyFunction &&) = default;
    MoveOnlyFunction(const MoveOnlyFunction &) = delete;
    MoveOnlyFunction & operator=(const MoveOnlyFunction &) = delete;

    explicit operator bool() const
    {
        return (bool) impl;
    }

    R operator()(Args... args)
    {
        return impl->invoke(std::forward<Args>(args)...);
    }
};

/**
 * Worker-thread stack size for evaluation. Evaluation is deeply
 * recursive, so worker threads need the same generous stack as
 * Determinate Nix's evaluator workers (60 MiB).
 */
constexpr size_t evalStackSize = 60 * 1024 * 1024;

struct Executor
{
    using work_t = MoveOnlyFunction<void()>;

    struct Item
    {
        std::promise<void> promise;
        work_t work;
    };

    struct State
    {
        std::multimap<uint64_t, Item> queue;
        std::vector<pthread_t> threads;
    };

    std::atomic_bool quit{false};

    const unsigned int evalCores;

    const bool enabled;

    const std::unique_ptr<InterruptCallback> interruptCallback;

    Sync<State> state_;

    std::condition_variable wakeup;

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

    /**
     * Whether parallel evaluation is enabled for the given settings
     * (eval-cores > 1 after the 0 == auto default is resolved). Does
     * not create the executor; see Evaluator::getExecutor().
     */
    static bool isEnabled(const EvalSettings & evalSettings)
    {
        return getEvalCores(evalSettings) > 1;
    }

    Executor(const EvalSettings & evalSettings);

    ~Executor();

    void createWorker(State & state);

    void worker();

    static void * workerEntry(void * arg);

    using WorkItems = std::vector<std::pair<work_t, uint8_t>>;

    std::vector<std::future<void>> spawn(WorkItems && items);

    [[gnu::tls_model("initial-exec")]]
    static thread_local bool amWorkerThread;
};

/**
 * Force a value deeply, using the evaluation executor.
 *
 * Only children whose forcing is likely to block on store/async I/O
 * (derivations, fetches, path realisation) are spawned as background
 * work items, so their blocking overlaps other work. Everything else
 * (pure computation) is forced inline on the walking thread, because
 * running many allocation-heavy thunks concurrently contends on the
 * Boehm GC's allocation lock and is slower than serial evaluation.
 *
 * The caller's subsequent sequential walk then reads already-forced
 * values (blocking on any in-flight work via the thunk waiter
 * machinery). Used by the JSON value printer (nix eval --json) and
 * flake check.
 */
void parallelForceDeep(EvalState & state, Value & v, PosIdx pos);

struct FutureVector
{
    Executor & executor;

    struct State
    {
        std::vector<std::future<void>> futures;
    };

    Sync<State> state_;

    ~FutureVector();

    void spawn(Executor::WorkItems && work);

    void spawn(uint8_t prioPrefix, Executor::work_t && work)
    {
        Executor::WorkItems items;
        items.emplace_back(std::move(work), prioPrefix);
        spawn(std::move(items));
    }

    void finishAll();
};

} // namespace nix
