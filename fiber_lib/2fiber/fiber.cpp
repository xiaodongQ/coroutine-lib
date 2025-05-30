#include "fiber.h"

static bool debug = false;

namespace sylar {

// 当前线程上的协程控制信息

// 下述几个线程局部变量，表示一个线程同时最多只能知道2个协程的上下文：主协程和当前运行协程（可能出现是同一个的时刻）
// 正在运行的协程（指针形式）
static thread_local Fiber* t_fiber = nullptr;
// 主协程（是一个智能指针）
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 调度协程（一般是主协程）
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 协程计数器
static std::atomic<uint64_t> s_fiber_id{0};
// 协程id
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber *f)
{
    t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()
{
    // 返回正在运行的协程
    if(t_fiber)
    {	
        return t_fiber->shared_from_this();
    }

    // 如果没有正在运行的协程，说明当前线程还未创建协程（否则肯定有一个主协程），则进行协程创建
        // Fiber()构造函数中会通过SetThis将本线程的t_fiber（当前运行的协程）设置为本协程
    std::shared_ptr<Fiber> main_fiber(new Fiber());
    // 线程第一次创建的协程默认设置为主协程
    t_thread_fiber = main_fiber;
    // 除非主动设置 主协程默认为调度协程
    t_scheduler_fiber = main_fiber.get();
    
    // 裸指针判断
    assert(t_fiber == main_fiber.get());
    return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber* f)
{
    t_scheduler_fiber = f;
}

// 当前运行中协程的id
uint64_t Fiber::GetFiberId()
{
    if(t_fiber)
    {
        return t_fiber->getId();
    }
    return (uint64_t)-1;
}

// 构造，创建一个协程
Fiber::Fiber()
{
    SetThis(this);
    m_state = RUNNING;
    
    // 获取用户上下文，保存到本协程上下文中
    if(getcontext(&m_ctx))
    {
        std::cerr << "Fiber() failed\n";
        pthread_exit(NULL);
    }
    
    // 全局的协程id
    m_id = s_fiber_id++;
    // 全局的协程个数
    s_fiber_count ++;
    if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler)
{
    m_state = READY;

    // 分配协程栈空间
    // 未指定则默认栈空间 128KB
    m_stacksize = stacksize ? stacksize : 128000;
    // 析构时会free掉
    m_stack = malloc(m_stacksize);

    // 获取用户上下文，保存到本协程上下文中
    if(getcontext(&m_ctx))
    {
        std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
        pthread_exit(NULL);
    }
    
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    // 绑定协程上下文和其执行函数：Fiber::MainFunc
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    
    // 全局的协程id
    m_id = s_fiber_id++;
    // 全局的协程个数
    s_fiber_count ++;
    if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

Fiber::~Fiber()
{
    s_fiber_count --;
    if(m_stack)
    {
        free(m_stack);
    }
    if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}

// 重复利⽤已结束的协程，复⽤其栈空间，创建新协程
void Fiber::reset(std::function<void()> cb)
{
    // 本协程为结束状态，且栈空间不为空
    assert(m_stack != nullptr&&m_state == TERM);

    // 就绪
    m_state = READY;
    // 设置运行函数
    m_cb = cb;

    // 获取当前协程上下文
    if(getcontext(&m_ctx))
    {
        std::cerr << "reset() failed\n";
        pthread_exit(NULL);
    }

    m_ctx.uc_link = nullptr;
    // 复用栈空间
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    // 绑定当前协程上下文和执行函数
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

void Fiber::resume()
{
    assert(m_state==READY);
    
    m_state = RUNNING;

    if(m_runInScheduler)
    {
        SetThis(this);
        if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_scheduler_fiber failed\n";
            pthread_exit(NULL);
        }
    }
    else
    {
        SetThis(this);
        if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

void Fiber::yield()
{
    assert(m_state==RUNNING || m_state==TERM);

    if(m_state!=TERM)
    {
        m_state = READY;
    }

    if(m_runInScheduler)
    {
        SetThis(t_scheduler_fiber);
        if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
        {
            std::cerr << "yield() to to t_scheduler_fiber failed\n";
            pthread_exit(NULL);
        }
    }
    else
    {
        SetThis(t_thread_fiber.get());
        if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
        {
            std::cerr << "yield() to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

void Fiber::MainFunc()
{
    // 当前运行的协程
    std::shared_ptr<Fiber> curr = GetThis();
    assert(curr!=nullptr);

    // 调用协程绑定的执行函数
    curr->m_cb();
    // 调用完成后协程结束，状态 TERM
    curr->m_cb = nullptr;
    curr->m_state = TERM;

    // 运行完毕 -> 让出执行权
    auto raw_ptr = curr.get();
    curr.reset(); 
    raw_ptr->yield(); 
}

}