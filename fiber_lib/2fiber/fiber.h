#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>
#include <memory>
#include <atomic>
#include <functional>
#include <cassert>
#include <ucontext.h>
#include <unistd.h>
#include <mutex>

namespace sylar {

// 协程类
// std::enable_shared_from_this 模板类允许通过 `shared_from_this()`成员函数来从当前实例获取 shared_ptr智能指针
  // 1、使用场景：
    // 当使用shared_ptr管理对象时，有时在对象内部还想获取对象自身的shared_ptr，比如成员函数将自身作为参数传递给其他函数，而该函数入参又是std::shared_ptr形式
    // 此时若使用this创建shared_ptr，会导致有两个控制引用计数的控制块，引用计数为0时会出现double free的情况
    // `std::enable_shared_from_this` 则可以和外部shared_ptr共享所有权，即共享同一个控制块
    // 典型应用场景：
        // 异步操作、回调、事件监听等场景下，需要确保在操作完成之前对象不会被销毁，如果传递this裸指针，则无法保证对象在回调时仍然存在
        // 而通过 `shared_from_this()` 获得一个 `shared_ptr`，则增加了引用计数，从而保证了对象的生命周期至少持续到回调完成。
  // 2、注意事项：
    // 使用 `shared_from_this()`时，必须保证此前已经有 std::shared_ptr 拥有该对象了，否则是未定义行为
    // 构造函数中不能使用`shared_from_this()`，因为还没有shared_ptr拥有该对象
    // 栈上分配的对象不能使用`shared_from_this()`
  // 3、原理：
    // `std::enable_shared_from_this`内部有一个`std::weak_ptr`成员，其在第一个shared_ptr创建时被初始化；
    // 并在`shared_from_this()`时，通过`weak_ptr`的`lock()`方法获取一个`shared_ptr`，它和已有的shared_ptr共享控制块
class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
    // 协程状态（简化为3种状态）
    enum State
    {
        READY,   // 就绪
        RUNNING, // 运行
        TERM     // 结束
    };

private:
    // 仅由GetThis()调用 -> 私有 -> 创建主协程  
    Fiber();

public:
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
    ~Fiber();

    // 重用一个协程
    // 重复利⽤已结束的协程，复⽤其栈空间，创建新的协程
    void reset(std::function<void()> cb);

    // 当前协程恢复执行
    // 协程交换，当前协程变为 RUNNING，正在运行的协程变为 READY
    void resume();

    // 当前协程让出执行权
    // 协程交换，当前协程变为 READY，上次resume时保存的协程变为 RUNNING
    void yield();

    uint64_t getId() const {return m_id;}
    State getState() const {return m_state;}

public:
    // 设置当前运行的协程
    static void SetThis(Fiber *f);

    // 得到当前运行的协程 
    static std::shared_ptr<Fiber> GetThis();

    // 设置调度协程（默认为主协程）
    static void SetSchedulerFiber(Fiber* f);
    
    // 得到当前运行的协程id
    static uint64_t GetFiberId();

    // 协程函数
    static void MainFunc();

private:
    // id
    uint64_t m_id = 0;
    // 栈大小
    uint32_t m_stacksize = 0;
    // 协程状态
    State m_state = READY;
    // 协程上下文
    ucontext_t m_ctx;
    // 协程栈指针
    void* m_stack = nullptr;
    // 协程函数
    std::function<void()> m_cb;
    // 是否让出执行权交给调度协程
    bool m_runInScheduler;

public:
    std::mutex m_mutex;
};

}

#endif

