#include "lix/libexpr/eval-settings.hh"
#include "lix/libexpr/parallel-eval.hh"
#include "lix/libutil/error.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace nix {

class ExecutorTest : public testing::Test
{};

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

} // namespace nix
