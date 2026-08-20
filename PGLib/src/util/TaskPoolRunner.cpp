#include "util/TaskPoolRunner.hpp"

#include "util/ExceptionHandler.hpp"
#include "util/Logger.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <cpptrace/from_current.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <thread>

using namespace std;

// STATICS
std::function<void()> TaskPoolRunner::s_exceptionCallback = nullptr;

TaskPoolRunner::TaskPoolRunner(const bool& multithread)
    : m_threadPool([]() -> unsigned int {
        auto availableThreads = std::thread::hardware_concurrency();
        if (availableThreads == 0) {
            availableThreads = 4;
        }
        return availableThreads - NUM_STATIC_THREADS;
    }())
    , m_multithread(multithread)
    , m_completedTasks(0)
{
}

void TaskPoolRunner::addTask(const function<void()>& task) { m_tasks.push_back(task); }

void TaskPoolRunner::runTasks()
{
    if (!m_multithread) {
        CPPTRACE_TRY
        {
            for (const auto& task : m_tasks) {
                task();
                m_completedTasks.fetch_add(1);
            }
        }
        CPPTRACE_CATCH(const exception& e)
        {
            ExceptionHandler::setException(e, cpptrace::from_current_exception().to_string());
            if (s_exceptionCallback) {
                s_exceptionCallback();
            }
        }

        return;
    }

    // Multithreading only beyond this point
    for (const auto& task : m_tasks) {
        boost::asio::post(m_threadPool, [this, task] {
            if (ExceptionHandler::hasException()) {
                // Exception already thrown, don't run thread
                return;
            }

            // Create log buffer
            Logger::startThreadedBuffer();

            CPPTRACE_TRY
            {
                task();
                m_completedTasks.fetch_add(1);
            }
            CPPTRACE_CATCH(const exception& e)
            {
                ExceptionHandler::setException(e, cpptrace::from_current_exception().to_string());
                if (s_exceptionCallback) {
                    s_exceptionCallback();
                }
            }

            // Flush log buffer
            Logger::flushThreadedBuffer();
        });
    }

    while (true) {
        // Check if all tasks are done
        if (m_completedTasks.load() >= m_tasks.size()) {
            // All tasks done
            break;
        }

        // If exception stop thread pool and throw. Real, confirmed bug found live (2026-08-19,
        // debugger stack trace + cdb attach): stop() only prevents NOT-YET-STARTED tasks from
        // running -- it does not (and cannot) retroactively make them "complete", so
        // m_completedTasks can never reach m_tasks.size() once even one task is abandoned this way.
        // Without this break, any real exception anywhere in a multithreaded run (this was first
        // hit via a std::filesystem::create_directories "filename too long" error, but the same
        // hang applies to any exception) turned a real, reportable error into a silent, permanent
        // hang instead -- the caller (main.cpp's own ExceptionHandler check after mainRunner)
        // never got a chance to run, since this loop never returned.
        if (ExceptionHandler::hasException()) {
            m_threadPool.stop();
            break;
        }

        // Sleep in between loops
        this_thread::sleep_for(chrono::milliseconds(LOOP_INTERVAL));
    }
}

void TaskPoolRunner::setExceptionCallback(const std::function<void()>& callback) { s_exceptionCallback = callback; }
