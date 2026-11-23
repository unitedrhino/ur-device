# 官方代码复用记录

- 上游：Seeed-Studio/SenseCAP-Watcher-Firmware
- 固定提交：`8e37f7ccd3d836bb99f483e0f6e74e8252406644`
- 原文件：`examples/factory_firmware/main/view/ui_manager/pm.c`、`pm.h`
- 许可：Apache-2.0，完整文本见 `SEEED-LICENSE`。原文件没有额外作者版权声明。
- 改编文件：`../watcher_page_manager.cc`、`page_manager.h`。
- 保留：按需初始化页面、焦点组重建、页面切换动画、页面复用。
- 修改：官方 LVGL 8 接口迁移到本仓 LVGL 9；去掉官方云和业务页面依赖；实例化管理器，增加输入互斥、无效对象检查和逐页焦点恢复。
- 未复用：Seeed 商标、整套图片动画、官方云通信和设备任务执行代码。

页面管理器已连接Watcher板级菜单与表盘，视图布局由本仓维护；首页大标题、右侧图标滚动及设置层级参考上述固定提交的ui_Page_Home.c和ui_Page_Set.c。
