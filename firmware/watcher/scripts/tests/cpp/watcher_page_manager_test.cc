// 直接编译生产页面管理器：验证官方 PM 移植后的页面、焦点及动画互斥行为。
#include "ui/page_manager.h"
#include <cassert>
#include <cstdio>
#include <limits>

// main 执行页面切换与边界断言；任一断言失败即返回非零进程状态。
int main() {
    lv_obj_t home_root, menu_root, first{&menu_root}, second{&menu_root};
    watcher::Page home, menu;
    home.screen = &home_root;
    menu.screen = &menu_root;
    menu.objects[0] = &first; menu.objects[1] = &second; menu.count = 2;
    watcher::PageManager manager;
    assert(manager.Open(home, 0));
    assert(!manager.Confirm(120));
    assert(!manager.Rotate(true, 120));
    assert(manager.Open(menu, 120));
    assert(!manager.Rotate(true, 200));
    assert(!manager.Confirm(200));
    assert(!manager.Open(home, 200));
    assert(manager.Confirm(240) == &first);
    assert(manager.Rotate(true, 240));
    assert(manager.Confirm(240) == &second);
    assert(manager.Open(home, 240, 0));
    assert(manager.Open(menu, 240, 0));
    assert(manager.Confirm(240) == &second);
    assert(manager.Rotate(true, 240));
    assert(manager.Confirm(240) == &first);
    assert(manager.Rotate(false, 240));
    assert(manager.Confirm(240) == &second);

    // 外部通话屏收尾后重新打开同一缓存菜单，必须重新装载正确屏幕。
    loaded_screen = &home_root;
    assert(manager.Open(menu, 240, 0));
    assert(loaded_screen == &menu_root);

    // 无效页面不能破坏原焦点、根对象或切页次数。
    const unsigned before = loads;
    watcher::Page bad;
    assert(!manager.Open(bad, 240));
    bad.initialize = [](watcher::Page&) { return false; };
    assert(!manager.Open(bad, 240));
    assert(manager.Current() == &menu);
    bad.screen = &home_root; bad.count = 13;
    assert(!manager.Open(bad, 240));
    bad.count = 1; bad.objects[0] = &first;
    assert(!manager.Open(bad, 240));  // 跨页对象。
    bad.screen = &menu_root; bad.count = 2; bad.objects[1] = &first;
    assert(!manager.Open(bad, 240));  // 重复对象。
    first.valid = false; bad.count = 1;
    assert(!manager.Open(bad, 240));
    first.valid = true;
    assert(loads == before);
    assert(manager.Confirm(240) == &second);

    // 惰性初始化必须完成后才使用根指针；重复打开不重建页面。
    static lv_obj_t lazy_root;
    static unsigned initialized = 0;
    watcher::Page lazy;
    lazy.initialize = [](watcher::Page& p) { ++initialized; p.screen = &lazy_root; return true; };
    assert(manager.Open(lazy, 240, 0));
    assert(loaded_screen == &lazy_root);
    assert(manager.Open(lazy, 240, 0));
    assert(initialized == 1);
    assert(!manager.Open(home, 240, 1001));

    // 从独占画面收回同一缓存页，也必须等待新动画结束，不能连续重启过渡。
    assert(manager.Open(menu,600,0));
    loaded_screen=&home_root;
    assert(manager.Open(menu,600,150));
    assert(!manager.Rotate(true,650));
    assert(!manager.Open(home,650));
    assert(manager.Confirm(750)==&second);
    assert(manager.Open(lazy,750,0));

    // 32 位毫秒时间回绕仍应阻止动画中途触发动作。
    assert(manager.Open(menu, std::numeric_limits<uint32_t>::max() - 50, 120));
    assert(!manager.Confirm(20));
    assert(manager.Confirm(70) == &second);
    std::puts("PASS watcher page manager");
}
