#include <algorithm>  // for_each
#include <iostream>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>  // condition_variable_any
#include <coroutine>
#include <exception>  // current_exception
#include <functional>  // bind_front
#include <future>  // promise, shared_future
#include <memory>  // enable_shared_from_this, make_shared, shared_ptr, weak_ptr
#include <mutex>  // lock_guard, unique_lock
#include <queue>
#include <stop_token>
#include <string_view>
#include <thread>  // jthread, sleep_for
#include <vector>

using namespace std;

struct executable : public std::enable_shared_from_this<executable> {
    virtual void execute() noexcept = 0;
    virtual ~executable() = default;
};
using executable_sptr = std::shared_ptr<executable>;

constinit const size_t DEFAULT_CONCURRENCY_LEVEL = 8;
template <size_t concurrency_level = DEFAULT_CONCURRENCY_LEVEL>
class thread_pool final {
public:
    static thread_pool& get_instance() {
        static thread_pool instance;
        return instance;
    }
    void schedule(executable_sptr task) {
        {
            std::lock_guard lock{ mutex_ };
            queue_.push(std::move(task));
        }
        cva_.notify_one();
    }
private:
    thread_pool() {
        for (size_t i{0}; i < concurrency_level; ++i) {
            threads_.emplace_back(std::bind_front(&thread_pool::run_thread, this));
        }
    }
    ~thread_pool() {
        std::ranges::for_each(threads_, [](auto& t) { t.request_stop(); });
        std::ranges::for_each(threads_, [](auto& t) { t.join(); });
    }
    void run_thread(std::stop_token stoken) {
        while (true) {
            std::unique_lock<std::mutex> lock{ mutex_};
            if (queue_.empty()) {
                cva_.wait(lock, stoken, [this]() {
                    return not queue_.empty();
                });
                if (stoken.stop_requested()) {
                    break;
                }
            }
            auto next{ std::move(queue_.front()) };
            queue_.pop();
            lock.unlock();
            next->execute();
        }
    }

    std::vector<std::jthread> threads_;
    std::mutex mutex_;
    std::queue<executable_sptr> queue_;
    std::condition_variable_any cva_;
};

template <typename T>
concept is_task = requires (T t) {
    typename T::promise_type;
    typename T::handle_type;
};

template <is_task task_t>
struct task_scheduler : public std::suspend_always {
    void await_suspend(task_t::handle_type handle) const noexcept {
        thread_pool<>::get_instance().schedule(handle.promise().get_state());
    }
};

template <typename T>
class task {
    class coroutine_promise;
    struct state;
public:
    auto get_result() {
        return shared_state_->get_result().get();
    }

    using promise_type = coroutine_promise;
    using handle_type = std::coroutine_handle<task<T>::promise_type>;
private:
    task(handle_type handle)
        : shared_state_{ std::make_shared<state>(handle) }
    {}

    std::shared_ptr<state> shared_state_;
};

template <typename T>
struct task<T>::state : public executable {
    state(handle_type handle)
        : handle_{ handle }
        , shared_future_{ result_.get_future() }
    {}
    void execute() noexcept override {
        handle_.resume();
    }
    auto get_result() {
        return shared_future_;
    }
    void set_result(T value) {
        result_.set_value(value);
    }
    handle_type handle_;
    std::promise<T> result_;
    std::shared_future<T> shared_future_;
};

template <typename T>
class task<T>::coroutine_promise {
private:
    std::weak_ptr<state> shared_state_;
public:
    auto get_return_object() {
        auto ret{ task<T>{ handle_type::from_promise(*this) } };
        shared_state_ = ret.shared_state_;
        return ret;
    }
    auto initial_suspend() {
        return task_scheduler<task<T>>{};
    }
    auto final_suspend() noexcept {
        get_state()->handle_ = nullptr;
        return std::suspend_never{};
    }
    auto return_value(T&& value) {
        get_state()->set_result(value);
    }
    auto unhandled_exception() {
        get_state()->result_.set_exception(std::current_exception());
    }
    auto get_state() {
        auto state{ shared_state_.lock() };
        assert(state);
        return state;
    }
};

task<int> async_add(int a, int b) {
    cout << "[debug] async_add(%d, %d) running on thread %s\n"<<
        a<<  b << std::this_thread::get_id();
    co_return a + b;
}

task<int> async_fib(int n) {
    cout << "[debug] async_fib(" << n << ") running on thread"
        << std::this_thread::get_id();

    if (n <= 2) {
        co_return 1;
    }

    int a = 1;
    int b = 1;

    // iterate computing fib(n)
    for (int i = 0; i < n - 2; ++i) {
        auto c{ async_add(a, b) };
        a = b;
        b = c.get_result();
    }
  

    co_return b;
}

void test_async_fib() {
    for (int i = 1; i < 10; ++i) {
        auto fib_task{ async_fib(i) };
        printf("async_fib(%d) returns %d\n", i, fib_task.get_result());
    }
}

int main() {
    printf("[debug] main() running on thread %s\n",
        std::this_thread::get_id());
    test_async_fib();
}
