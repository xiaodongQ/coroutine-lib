#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <assert.h>
#include <functional>
#include <mutex>

namespace sylar {

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer> 
{
    // 声明 TimerManager 是 Timer 的友元（TimerManager 可以访问 Timer 的私有成员）
    friend class TimerManager;
public:
    // 从时间堆中删除timer
    bool cancel();
    // 刷新timer
    bool refresh();
    // 重设timer的超时时间
    bool reset(uint64_t ms, bool from_now);

private:
    // 此处构造定义为了私有，可通过友元类（friend）TimerManager来访问
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);
 
private:
    // 是否循环
    bool m_recurring = false;
    // 超时时间
    uint64_t m_ms = 0;
    // 绝对超时时间
    std::chrono::time_point<std::chrono::system_clock> m_next;
    // 超时时触发的回调函数
    std::function<void()> m_cb;
    // 管理此timer的管理器
    TimerManager* m_manager = nullptr;

private:
    // 实现最小堆的比较函数
    struct Comparator 
    {
        // 实现中是由小到大：lhs->m_next < rhs->m_next。默认情况就是`std::less`，也是从小到大升序排序。
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };
};

class TimerManager 
{
    // 声明 Timer 是 TimerManager 的友元（Timer 可以访问 TimerManager 的私有成员）
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();

    // 添加timer
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    // 添加条件timer
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

    // 拿到堆中最近的超时时间
    uint64_t getNextTimer();

    // 取出所有超时定时器的回调函数
    void listExpiredCb(std::vector<std::function<void()>>& cbs);

    // 堆中是否有timer
    bool hasTimer();

protected:
    // 当一个最早的timer加入到堆中 -> 调用该函数
    virtual void onTimerInsertedAtFront() {};

    // 添加timer
    void addTimer(std::shared_ptr<Timer> timer);

private:
    // 当系统时间改变时 -> 调用该函数
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex;
    // 时间堆。根据超时时间由小到大排序
    // 此处用set模拟堆，begin作为堆顶（最小堆，堆顶是最小元素）。而没有用std::priority_queue优先级队列
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;
    // 在下次getNextTime()执行前 onTimerInsertedAtFront()是否已经被触发了 -> 在此过程中 onTimerInsertedAtFront()只执行一次
    bool m_tickled = false;
    // 上次检查系统时间是否回退的绝对时间
    std::chrono::time_point<std::chrono::system_clock> m_previouseTime;
};

}

#endif