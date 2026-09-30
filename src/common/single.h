#pragma once

#include"common/common.h"

// Meyers 单例（函数内静态变量）：
//   1) 首次调用时按需构造，彻底避免跨编译单元的静态初始化顺序问题
//      （旧实现用静态数据成员，别的 TU 的全局对象在静态初始化期取实例会拿到空 shared_ptr → 段错误）；
//   2) C++11 起函数内静态变量的初始化是线程安全的（magic static），并发首次调用无数据竞争；
//   3) 不经过 shared_ptr 删除器，因此 T 的构造/析构可以保持 private
//      （旧实现用 shared_ptr + new T()，delete 发生在 libstdc++ 内部，友元声明无效 → 编译不过）。
//
// 子类写法：
//   class Foo : public Singleton<Foo> {
//       friend class Singleton<Foo>;
//   public:
//       static Foo& getInstance();
//   private:
//       Foo();
//       ~Foo();
//   };
template<typename T>
class Singleton{
public:
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(Singleton&&) = delete;

    static T& getInstance(){
        static T instance;
        return instance;
    }

protected:
    Singleton() = default;           // 允许子类调用，外部不能调用
    ~Singleton() = default;
};
