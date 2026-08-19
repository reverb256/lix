#include "lix/libexpr/eval-error.hh"
#include "lix/libexpr/eval-inline.hh"
#include "lix/libexpr/eval-settings.hh"
#include "lix/libexpr/parallel-eval.hh"
#include "lix/libutil/error.hh"
#include "lix/libutil/logging.hh"

#include <gtest/gtest.h>

#include "tests/libexpr.hh"

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace nix {

class ExecutorTest : public testing::Test
{};

/**
 * Logger that counts occurrences of a marker string, used to prove
 * that a thunk is evaluated exactly once under concurrent forcing.
 */
struct CountingTraceLogger : Logger
{
    std::atomic<size_t> & count;

    explicit CountingTraceLogger(std::atomic<size_t> & count) : count(count) {}

    BufferState log(Verbosity, std::string_view s) override
    {
        if (s.find("EVALED") != std::string_view::npos) {
            count.fetch_add(1);
        }
        return BufferState::HasSpace;
    }

    BufferState logEI(const ErrorInfo & ei) override
    {
        return BufferState::HasSpace;
    }
};

/**
 * Installs a counting logger for the duration of the scope.
 */
struct ScopedTraceCounter
{
    std::atomic<size_t> count{0};
    Logger * old;
    CountingTraceLogger counting;

    ScopedTraceCounter() : old(logger), counting(count)
    {
        logger = &counting;
    }

    ~ScopedTraceCounter()
    {
        logger = old;
    }
};

class ParallelEvalTest : public LibExprTest
{
protected:
    /**
     * Builds a fresh, slow thunk: `let go = n: if n == 0 then
     * builtins.trace "EVALED" 42 else go (n - 1); in { a = go 9500; }`
     * (kept under the default maxCallDepth of 10000). The trace fires
     * exactly once per evaluation, so counting "EVALED" proves how
     * many threads evaluated the thunk.
     */
    Value slowThunk()
    {
        auto v = eval(
            "let go = n: if n == 0 then builtins.trace \"EVALED\" 42 else go (n - 1); "
            "in { a = go 9500; }",
            false
        );
        return v;
    }

    /**
     * Sets the global eval-cores setting for the duration of the scope,
     * restoring it afterwards. parallelForceDeep reads this setting
     * lazily (when the executor is first created), so it must be set
     * before the call, not before the fixture is constructed.
     */
    struct ScopedEvalCores
    {
        unsigned long old;

        explicit ScopedEvalCores(unsigned long cores) : old(evalSettings.evalCores.get())
        {
            evalSettings.set("eval-cores", std::to_string(cores));
        }

        ~ScopedEvalCores()
        {
            evalSettings.set("eval-cores", std::to_string(old));
        }
    };
};

TEST_F(ParallelEvalTest, parallelForceDeepForcesDeepStructure)
{
    ScopedEvalCores guard(4);

    auto v = eval(
        "{ a = { b = { c = 1 + 2; }; d = [ 4 5 (6 + 7) ]; }; "
        "e = { f = \"hello\"; }; g = map (x: x * 2) [ 1 2 3 ]; }",
        false
    );

    parallelForceDeep(state, v, noPos);

    // v itself is forced to an attrset by parallelForceDeep.
    ASSERT_TRUE(v.type() == nAttrs);

    // Force every leaf (blocking on any in-flight workers) and check the
    // values are correct.
    const Attr * aAttr = v.attrs()->get(createSymbol("a"));
    ASSERT_NE(aAttr, nullptr);
    state.forceValue(aAttr->value, noPos); // blocks on the in-flight worker
    ASSERT_TRUE(aAttr->value.type() == nAttrs);
    const Attr * bAttr = aAttr->value.attrs()->get(createSymbol("b"));
    ASSERT_NE(bAttr, nullptr);
    state.forceValue(bAttr->value, noPos);
    ASSERT_TRUE(bAttr->value.type() == nAttrs);
    state.forceValue(bAttr->value.attrs()->get(createSymbol("c"))->value, noPos);
    ASSERT_EQ(bAttr->value.attrs()->get(createSymbol("c"))->value.integer().value, 3);

    const Attr * dAttr = aAttr->value.attrs()->get(createSymbol("d"));
    ASSERT_NE(dAttr, nullptr);
    state.forceValue(dAttr->value, noPos);
    ASSERT_TRUE(dAttr->value.type() == nList);
    state.forceValue(dAttr->value.listElems()[2], noPos);
    ASSERT_EQ(dAttr->value.listElems()[2].integer().value, 13);

    const Attr * gAttr = v.attrs()->get(createSymbol("g"));
    ASSERT_NE(gAttr, nullptr);
    state.forceValue(gAttr->value, noPos);
    ASSERT_TRUE(gAttr->value.type() == nList);
    state.forceValue(gAttr->value.listElems()[1], noPos);
    ASSERT_EQ(gAttr->value.listElems()[1].integer().value, 4);
}

TEST_F(ParallelEvalTest, parallelForceDeepEvaluatesEachThunkOnce)
{
    ScopedEvalCores guard(4);
    ScopedTraceCounter counter;

    auto v = eval(
        "let go = n: if n == 0 then builtins.trace \"EVALED\" 42 else go (n - 1); "
        "in { a = go 9500; b = go 9500; c = go 9500; }",
        false
    );

    parallelForceDeep(state, v, noPos);

    // Force each attr (blocking on workers) and verify each was
    // evaluated exactly once, even under parallel forcing.
    for (auto * name : {"a", "b", "c"}) {
        state.forceValue(v.attrs()->get(createSymbol(name))->value, noPos);
        ASSERT_EQ(v.attrs()->get(createSymbol(name))->value.integer().value, 42);
    }
    ASSERT_EQ(counter.count.load(), 3);
}

TEST_F(ParallelEvalTest, builtinsParallelForcesListElementsInBackground)
{
    // The primop is gated behind the parallel-eval experimental feature
    // and registered at EvalState construction time, so enable it and
    // build a fresh evaluator for this test.
    experimentalFeatureSettings.set("experimental-features", "nix-command parallel-eval");
    evalSettings.set("eval-cores", "4");
    ScopedTraceCounter counter;

    Evaluator ev(aio, {}, store);
    auto st = ev.begin(aio);
    EvalState & s = *st;

    // `x` depends on the list, so forcing it waits for (and joins with)
    // the background forces spawned by builtins.parallel. Each element
    // must still be evaluated exactly once.
    auto v = s.eval(ev.parseExprFromString(
        "let xs = [ (builtins.trace \"EVALED\" 1) (builtins.trace \"EVALED\" 2) ]; "
        "in builtins.parallel xs (builtins.foldl' (a: b: a + b) 0 xs)",
        CanonPath::root
    ));
    s.forceValue(v, noPos);
    ASSERT_TRUE(v.type() == nInt);
    ASSERT_EQ(v.integer().value, 3);
    ASSERT_EQ(counter.count.load(), 2);

    experimentalFeatureSettings.set("experimental-features", "");
    evalSettings.set("eval-cores", "0");
}

TEST_F(ExecutorTest, spawnRunsAllWorkItems)
{
    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "4");
    Executor executor(evalSettings);
    ASSERT_TRUE(executor.enabled);

    constexpr size_t n = 100;
    std::atomic<size_t> count{0};

    Executor::WorkItems items;
    for (size_t i = 0; i < n; ++i) {
        items.emplace_back([&count] { count.fetch_add(1); }, 0);
    }

    auto futures = executor.spawn(std::move(items));
    for (auto & future : futures) {
        future.get();
    }

    ASSERT_EQ(count.load(), n);
}

TEST_F(ExecutorTest, spawnRunsAcrossThreads)
{
    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "4");
    Executor executor(evalSettings);

    std::mutex mutex;
    std::set<std::thread::id> threadIds;

    Executor::WorkItems items;
    for (size_t i = 0; i < 200; ++i) {
        items.emplace_back(
            [&] {
                std::lock_guard lock(mutex);
                threadIds.insert(std::this_thread::get_id());
            },
            0
        );
    }

    auto futures = executor.spawn(std::move(items));
    for (auto & future : futures) {
        future.get();
    }

    ASSERT_GT(threadIds.size(), 1);
}

TEST_F(ExecutorTest, futureVectorFinishesAll)
{
    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "2");
    Executor executor(evalSettings);

    std::atomic<size_t> count{0};
    {
        FutureVector futures(executor);
        for (size_t i = 0; i < 50; ++i) {
            futures.spawn(0, [&count] { count.fetch_add(1); });
        }
        futures.finishAll();
    }
    ASSERT_EQ(count.load(), 50);
}

TEST_F(ExecutorTest, exceptionsPropagate)
{
    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "2");
    Executor executor(evalSettings);

    Executor::WorkItems items;
    items.emplace_back([] { throw Error("boom"); }, 0);

    auto futures = executor.spawn(std::move(items));
    ASSERT_THROW(futures[0].get(), Error);
}

TEST_F(ExecutorTest, singleCoreDisables)
{
    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "1");
    Executor executor(evalSettings);
    ASSERT_FALSE(executor.enabled);
}

TEST_F(ExecutorTest, workerThreadsAreMarked)
{
    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "2");
    Executor executor(evalSettings);

    std::atomic<bool> sawWorker{false};
    Executor::WorkItems items;
    items.emplace_back(
        [&] {
            if (Executor::amWorkerThread) {
                sawWorker = true;
            }
        },
        0
    );
    auto futures = executor.spawn(std::move(items));
    futures[0].get();
    ASSERT_TRUE(sawWorker.load());
}

TEST_F(ParallelEvalTest, concurrentForceOfSharedThunkEvaluatesOnce)
{
    ScopedTraceCounter counter;

    // Several rounds of fresh thunks to stress the state machine.
    for (int round = 0; round < 20; ++round) {
        counter.count = 0;
        auto v = slowThunk();
        auto a = v.attrs()->get(createSymbol("a"));
        ASSERT_THAT(a->value, IsThunk());

        std::atomic<size_t> ok{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                state.forceValue(a->value, noPos);
                if (a->value.type() == nInt && a->value.integer().value == 42) {
                    ok.fetch_add(1);
                }
            });
        }
        for (auto & t : threads) {
            t.join();
        }

        ASSERT_EQ(ok.load(), 4);
        // Exactly one thread evaluated the thunk; the rest waited for
        // the winner.
        ASSERT_EQ(counter.count.load(), 1);
    }
}

TEST_F(ParallelEvalTest, concurrentForceOfResolvedThunk)
{
    auto v = slowThunk();
    auto a = v.attrs()->get(createSymbol("a"));

    // Resolve once on the main thread; it is now memoized.
    state.forceValue(a->value, noPos);
    ASSERT_EQ(a->value.integer().value, 42);

    std::atomic<size_t> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            state.forceValue(a->value, noPos);
            if (a->value.integer().value == 42) {
                ok.fetch_add(1);
            }
        });
    }
    for (auto & t : threads) {
        t.join();
    }
    ASSERT_EQ(ok.load(), 4);
}

TEST_F(ParallelEvalTest, executorWorkersForceSharedThunk)
{
    ScopedTraceCounter counter;

    EvalSettings evalSettings;
    evalSettings.set("eval-cores", "4");
    Executor executor(evalSettings);

    auto v = slowThunk();
    auto a = v.attrs()->get(createSymbol("a"));
    ASSERT_THAT(a->value, IsThunk());

    std::atomic<size_t> ok{0};
    Executor::WorkItems items;
    for (int i = 0; i < 4; ++i) {
        items.emplace_back(
            [&] {
                state.forceValue(a->value, noPos);
                if (a->value.integer().value == 42) {
                    ok.fetch_add(1);
                }
            },
            0
        );
    }
    auto futures = executor.spawn(std::move(items));
    for (auto & future : futures) {
        future.get();
    }

    ASSERT_EQ(ok.load(), 4);
    ASSERT_EQ(counter.count.load(), 1);
}

TEST_F(ParallelEvalTest, sameThreadRecursionStillErrors)
{
    // Same-thread infinite recursion must still raise an error (it must
    // not turn into a cross-thread wait that deadlocks).
    ASSERT_THROW(eval("let x = x; in x"), InfiniteRecursionError);
    ASSERT_THROW(eval("rec { a = b; b = a; }.a"), InfiniteRecursionError);
}

} // namespace nix
