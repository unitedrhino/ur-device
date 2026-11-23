// Watcher 菜单纯逻辑：输入与业务动作分离，危险操作二次确认，活动流程不自动退出。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watcher {

// NetworkErrorText 将持久化非敏感错误码转换为圆屏短提示，不展示 SSID 或凭据。
inline const char* NetworkErrorText(uint8_t error) {
    switch (error) {
        case 1:
            return "重启中断，保留原网络";
        case 2:
            return "等待超时，保留原网络";
        case 3:
            return "已取消，保留原网络";
        case 4:
            return "平台鉴权失败";
        case 5:
            return "网络连接失败";
        case 6:
            return "配置存储失败";
        default:
            return "请返回网络设置重试";
    }
}

// MenuPage 标识菜单层级；Home承载空闲表盘和活动对话，Main承载设置。
enum class MenuPage {
    Home,
    Main,
    Network,
    Volume,
    Brightness,
    Info,
    Power,
    ConfirmPower,
    ConfirmReset,
    ConfirmNetwork,
    NetworkProgress,
    NetworkResult,
    ConfirmBind,
    Mirror,
    Contacts,
    Contact,
    Skins  // 设置中的皮肤选择页，旧页面编号保持稳定。
};
// MenuInput 来自按钮、滚轮或触摸；Back 只返回，不像主页长按那样打开菜单。
enum class MenuInput {
    Click,
    DoubleClick,
    LongPress,
    Next,
    Previous,
    Tick,
    NetworkDone,
    Back,
    MirrorOn,
    MirrorOff,
    SwipeNext,      // 上滑只切换菜单，不复用通话音量动作。
    SwipePrevious  // 下滑只切换菜单，不进入或确认条目。
};
// TouchBackIndex 是触摸队列中的返回标记，不对应列表条目，沿用页面代次和业务门禁。
// 与所有主菜单和分页条目索引保持分离，防止新增条目被误判为返回。
inline constexpr size_t TouchBackIndex = 32;
// MenuTarget 为稳定语义目标，布局调整和联系人翻页不能改变按钮含义。
enum class MenuTarget : size_t {
    Chat,
    Directory,
    Photo,
    Mirror,
    Settings,
    Network,
    Volume,
    Brightness,
    Info,
    Power,
    Cancel,
    Confirm,
    ChangeWifi,
    Bind,
    Shutdown,
    Reset,
    Finish,
    Refresh,
    PreviousPage,
    NextPage,
    Contact0,
    Contact1,
    Contact2,
    Contact3,
    Voice,
    Video,
    Decrease,
    Increase,
    Skins = 28,    // 新动作使用返回标记之前的保留编号。
    SkinGreen,
    SkinBlue,
    SkinLight,
    Back = TouchBackIndex
};
// MenuAction 返回需要由主任务执行的动作；CallVolumeDelta 的参数是相对音量变化。
enum class MenuAction {
    None,
    Chat,
    Photo,
    CloseChat,
    ChangeNetwork,
    Volume,
    CallVolumeDelta,
    Brightness,
    Power,
    Reset,
    Visibility,
    ConfirmNetwork,
    CancelNetwork,
    NetworkTimeout,
    ProvisionBind,
    FinishNetwork,
    Mirror,
    LoadContacts,
    DialVoice,
    DialVideo,
    Skin  // 参数为皮肤索引，通过应用任务保存主题选择。
};

// MenuModel 仅由显示任务持有；固定字段避免滚轮事件造成反复堆分配。
class MenuModel {
public:
    // TargetAt 返回焦点条目的稳定含义；无效索引只有返回语义，调用者须先校验Count。
    MenuTarget TargetAt(size_t index) const {
        if (page_ == MenuPage::Home)
            return static_cast<MenuTarget>(index);
        if (page_ == MenuPage::Main) {
            constexpr MenuTarget targets[] = {MenuTarget::Back,   MenuTarget::Network,
                                              MenuTarget::Volume, MenuTarget::Brightness,
                                              MenuTarget::Info,   MenuTarget::Power, MenuTarget::Skins};
            return index < 7 ? targets[index] : MenuTarget::Back;
        }
        if (page_ == MenuPage::Skins)
            return index == 0 ? MenuTarget::Back
                              : static_cast<MenuTarget>(size_t(MenuTarget::SkinGreen) + index - 1);
        if (page_ == MenuPage::Contacts) {
            if (index == 0)
                return MenuTarget::Back;
            if (index == 1)
                return MenuTarget::Refresh;
            if (index < contacts_count_ + 2)
                return static_cast<MenuTarget>(size_t(MenuTarget::Contact0) + index - 2);
            if (contacts_offset_ > 0 && index == contacts_count_ + 2)
                return MenuTarget::PreviousPage;
            return MenuTarget::NextPage;
        }
        if (page_ == MenuPage::Contact)
            return index == 0   ? MenuTarget::Back
                   : index == 1 ? MenuTarget::Voice
                                : MenuTarget::Video;
        if (page_ == MenuPage::Volume || page_ == MenuPage::Brightness)
            return index == 0   ? MenuTarget::Back
                   : index == 1 ? MenuTarget::Decrease
                                : MenuTarget::Increase;
        if (page_ == MenuPage::Network)
            return index == 0   ? MenuTarget::Back
                   : index == 1 ? MenuTarget::ChangeWifi
                                : MenuTarget::Bind;
        if (page_ == MenuPage::Power)
            return index == 0   ? MenuTarget::Back
                   : index == 1 ? MenuTarget::Shutdown
                                : MenuTarget::Reset;
        if (page_ == MenuPage::ConfirmPower || page_ == MenuPage::ConfirmReset ||
            page_ == MenuPage::ConfirmNetwork || page_ == MenuPage::ConfirmBind)
            return index == 0 ? MenuTarget::Cancel : MenuTarget::Confirm;
        if (page_ == MenuPage::NetworkResult && network_saved_)
            return MenuTarget::Finish;
        return MenuTarget::Back;
    }
    // TouchTarget 以语义目标确认原页面，底部翻页先消费本批次的两条切片。
    MenuAction TouchTarget(MenuTarget target, uint32_t revision, uint32_t now, bool asleep = false,
                           bool busy = false) {
        if (revision != revision_ || asleep || busy)
            return MenuAction::None;
        if (target == MenuTarget::Back)
            return TouchBack(revision, now);
        if (page_ == MenuPage::Contacts && !contacts_loading_) {
            const auto value = size_t(target);
            if (value >= size_t(MenuTarget::Contact0) && value <= size_t(MenuTarget::Contact3) &&
                (value - size_t(MenuTarget::Contact0) < contacts_slice_ ||
                 value - size_t(MenuTarget::Contact0) >= contacts_slice_ + 2))
                return MenuAction::None;
            if (target == MenuTarget::NextPage && contacts_slice_ == 0 && contacts_count_ > 2) {
                contacts_slice_ = 2;
                focus_ = 4;
                ++revision_;
                touched_ = now;
                return MenuAction::None;
            }
            if (target == MenuTarget::PreviousPage && contacts_slice_ > 0) {
                contacts_slice_ = 0;
                focus_ = 2;
                ++revision_;
                touched_ = now;
                return MenuAction::None;
            }
        }
        for (size_t index = 0; index < Count(); ++index)
            if (TargetAt(index) == target)
                return TouchItem(index, revision, now, asleep, busy);
        return MenuAction::None;
    }
    // SetConversationActive 从原子设备状态快照更新主页模式，作废旧表盘输入。
    bool SetConversationActive(bool active) {
        if (conversation_active_ == active)
            return false;
        conversation_active_ = active;
        if (page_ == MenuPage::Home)
            ++revision_;
        return true;
    }
    bool ConversationActive() const {
        return conversation_active_;
    }                                                         // 活动对话使用原音量和聊天按键。
    size_t ContactsSlice() const { return contacts_slice_; }  // 当前四项批次中可见两项的起点。

    // TouchBack 仅在原页面且当前可操作时返回；独立于条目索引，非法条目仍不能执行动作。
    MenuAction TouchBack(uint32_t revision, uint32_t now, bool asleep = false, bool busy = false) {
        if (revision != revision_ || asleep || busy)
            return MenuAction::None;
        return Handle(MenuInput::Back, now);
    }
    // TouchItem 使用按下时的页面代次确认条目；翻页后的抬手、隐藏行和唤醒手势均不能执行动作。
    MenuAction TouchItem(size_t index, uint32_t revision, uint32_t now, bool asleep = false,
                         bool busy = false) {
        if (revision != revision_ || index >= Count() || page_ == MenuPage::NetworkProgress ||
            asleep || busy)
            return MenuAction::None;
        if (page_ == MenuPage::Home) {
            if (conversation_active_) {
                if (index == 4) {
                    Open(MenuPage::Main);
                    return MenuAction::CloseChat;
                }
                return index == 0 ? MenuAction::Chat : MenuAction::None;
            }
            home_focus_ = focus_ = index;
            return Handle(MenuInput::Click, now);
        }
        // 先检查绝对确认时限，不能让触摸选中动作把过期确认变成成功。
        if (page_ == MenuPage::ConfirmNetwork && uint32_t(now - confirmation_started_) >= 30000)
            return Handle(MenuInput::Tick, now);
        // 长时间停留后的迟到点击不能绕过普通菜单自动关闭；2 分钟给足滚动和阅读时间。
        if (uint32_t(now - touched_) >= 120000)
            return Handle(MenuInput::Tick, now);
        if ((page_ == MenuPage::Volume || page_ == MenuPage::Brightness) && index > 0) {
            // 触摸减/加按钮保持文字语义；亮度右增左减，音量沿用左增右减。
            const bool next = page_ == MenuPage::Brightness ? index == 2 : index == 1;
            return Handle(next ? MenuInput::Next : MenuInput::Previous, now);
        }
        focus_ = index;
        return Handle(MenuInput::Click, now);
    }
    // Revision 标识页面生命周期；同一个页面退出再进入也视为新的确认上下文。
    uint32_t Revision() const { return revision_; }
    // TouchActivity 仅延长普通菜单空闲时间，不能延长远端确认的绝对有效期。
    void TouchActivity(uint32_t now) { touched_ = now; }
    // Handle 消费一次输入；睡眠后的首次输入仅唤醒，活动流程时冻结菜单动作。
    MenuAction Handle(MenuInput input, uint32_t now, bool asleep = false, bool busy = false) {
        // 独占流程的取消由板级事务执行；菜单不得超时返回或透传对话/拍照。
        if (page_ == MenuPage::NetworkProgress)
            return MenuAction::None;
        // 保存成功仍处于独占 BLE 启动；完成必须交给板级重启，不能提前开放聊天或取消事务。
        if (page_ == MenuPage::NetworkResult && network_saved_) {
            // 普通启动仍会读到持久化成功终态；板级确认无需重启时，显式关闭结果页。
            if (input == MenuInput::NetworkDone) {
                Home();
                return MenuAction::None;
            }
            if (asleep || busy || network_finishing_)
                return MenuAction::None;
            if (input == MenuInput::Click || input == MenuInput::LongPress ||
                input == MenuInput::Back) {
                network_finishing_ = true;
                return MenuAction::FinishNetwork;
            }
            return MenuAction::None;
        }
        if (page_ == MenuPage::ConfirmNetwork && uint32_t(now - confirmation_started_) >= 30000) {
            Home();
            return MenuAction::NetworkTimeout;
        }
        if (asleep) {
            touched_ = now;
            return MenuAction::None;
        }
        if (input == MenuInput::Tick) {
            // 镜子是活动预览，不按普通设置页超时归位，以免页面退出而采集仍在运行。
            if (!busy && page_ != MenuPage::Home && page_ != MenuPage::Mirror &&
                uint32_t(now - touched_) >= 120000)
                Home();
            return MenuAction::None;
        }
        touched_ = now;
        if (busy)
            return MenuAction::None;
        // 滑动只允许空闲表盘和普通列表；确认页、通讯录拨号与活动对话不消费它。
        if (input == MenuInput::SwipeNext || input == MenuInput::SwipePrevious) {
            if ((page_ != MenuPage::Home || conversation_active_) && page_ != MenuPage::Main &&
                page_ != MenuPage::Network && page_ != MenuPage::Power && page_ != MenuPage::Skins)
                return MenuAction::None;
            input = input == MenuInput::SwipeNext ? MenuInput::Next : MenuInput::Previous;
        }
        // 调试启停与菜单点击共用一个页面状态；动作值由调用方根据最终页面生成。
        if (input == MenuInput::MirrorOn) {
            if (page_ == MenuPage::Mirror)
                return MenuAction::None;
            Open(MenuPage::Mirror);
            return MenuAction::Mirror;
        }
        if (input == MenuInput::MirrorOff) {
            if (page_ != MenuPage::Mirror)
                return MenuAction::None;
            Home();
            return MenuAction::Mirror;
        }
        if (input == MenuInput::Back && page_ == MenuPage::Home)
            return MenuAction::None;
        if (input == MenuInput::LongPress || input == MenuInput::Back) {
            if (page_ == MenuPage::ConfirmNetwork) {
                Home();
                return MenuAction::CancelNetwork;
            }
            if (page_ == MenuPage::Home) {
                Open(MenuPage::Main);
                return MenuAction::CloseChat;
            }
            if (page_ == MenuPage::Main)
                Home();
            else if (page_ == MenuPage::ConfirmPower || page_ == MenuPage::ConfirmReset)
                Open(MenuPage::Power);
            else if (page_ == MenuPage::Contact) {
                Open(MenuPage::Contacts);
                focus_ = selected_contact_ + 2;
            } else if (page_ == MenuPage::ConfirmBind)
                Open(MenuPage::Network);
            else if (page_ == MenuPage::Mirror) {
                Home();
                return MenuAction::Mirror;
            } else if (page_ == MenuPage::Contacts)
                Home();
            else
                Open(MenuPage::Main);
            return MenuAction::None;
        }
        if (page_ == MenuPage::Home) {
            if (input == MenuInput::DoubleClick)
                return MenuAction::Photo;
            if (conversation_active_) {
                if (input == MenuInput::Click)
                    return MenuAction::Chat;
                if (input == MenuInput::Next || input == MenuInput::Previous) {
                    volume_ = Clamp(volume_ + (input == MenuInput::Next ? -5 : 5));
                    return MenuAction::Volume;
                }
                return MenuAction::None;
            }
            if (input == MenuInput::Next || input == MenuInput::Previous) {
                home_focus_ = focus_ = (focus_ + 5 + (input == MenuInput::Next ? 1 : -1)) % 5;
            } else if (input == MenuInput::Click) {
                switch (focus_) {
                    case 0:
                        return MenuAction::Chat;
                    case 1:
                        return BeginContacts(0);
                    case 2:
                        return MenuAction::Photo;
                    case 3:
                        Open(MenuPage::Mirror);
                        return MenuAction::Mirror;
                    case 4:
                        Open(MenuPage::Main);
                        return MenuAction::CloseChat;
                }
            }
            return MenuAction::None;
        }
        // 镜子双击关闭预览并回主页；其他菜单双击不能透传到拍照入口。
        if (page_ == MenuPage::Mirror && input == MenuInput::DoubleClick) {
            Home();
            return MenuAction::Mirror;
        }
        if (input == MenuInput::DoubleClick)
            return MenuAction::None;
        if (input == MenuInput::Next || input == MenuInput::Previous) {
            const int delta = input == MenuInput::Next ? 1 : -1;
            if (page_ == MenuPage::Volume || page_ == MenuPage::Brightness) {
                int& value = page_ == MenuPage::Volume ? volume_ : brightness_;
                // 亮度右滚增加、左滚减小；音量保留用户已确认的主页方向。
                value += (page_ == MenuPage::Brightness ? delta : -delta) * 5;
                if (value < 0)
                    value = 0;
                if (value > 100)
                    value = 100;
                return page_ == MenuPage::Volume ? MenuAction::Volume : MenuAction::Brightness;
            }
            focus_ = (focus_ + Count() + delta) % Count();
            if (page_ == MenuPage::Contacts && focus_ >= 2 && focus_ < contacts_count_ + 2)
                contacts_slice_ = ((focus_ - 2) / 2) * 2;
            return MenuAction::None;
        }
        if (input != MenuInput::Click)
            return MenuAction::None;
        switch (page_) {
            case MenuPage::NetworkResult:
                if (focus_ == 0)
                    Home();
                else
                    Open(MenuPage::Network);
                break;
            case MenuPage::ConfirmNetwork: {
                const auto action =
                    focus_ == 1 ? MenuAction::ConfirmNetwork : MenuAction::CancelNetwork;
                Home();
                return action;
            }
            case MenuPage::Main:
                switch (focus_) {
                    case 0:
                        Home();
                        break;
                    case 1:
                        Open(MenuPage::Network);
                        break;
                    case 2:
                        Open(MenuPage::Volume);
                        break;
                    case 3:
                        Open(MenuPage::Brightness);
                        break;
                    case 4:
                        Open(MenuPage::Info);
                        break;
                    case 5:
                        Open(MenuPage::Power);
                        break;
                    case 6:
                        Open(MenuPage::Skins);
                        focus_ = skin_index_ + 1;
                        break;
                }
                break;
            case MenuPage::Skins:
                if (focus_ == 0) {
                    Open(MenuPage::Main);
                } else {
                    skin_index_ = focus_ - 1;
                    ++revision_;  // 换肤作废持续触点及旧配色页面的排队点击。
                    return MenuAction::Skin;
                }
                break;
            case MenuPage::Contacts:
                if (focus_ == 0) {
                    Home();
                    break;
                }
                if (contacts_loading_)
                    break;
                if (focus_ == 1)
                    return BeginContacts(0);
                if (focus_ < contacts_count_ + 2) {
                    selected_contact_ = focus_ - 2;
                    Open(MenuPage::Contact);
                } else if (contacts_offset_ > 0 && focus_ == contacts_count_ + 2) {
                    return BeginContacts(contacts_offset_ - 4, 2);
                } else if (contacts_more_) {
                    return BeginContacts(contacts_offset_ + 4);
                }
                break;
            case MenuPage::Contact:
                if (focus_ == 0) {
                    Open(MenuPage::Contacts);
                    focus_ = selected_contact_ + 2;
                    break;
                }
                return focus_ == 1 ? MenuAction::DialVoice : MenuAction::DialVideo;
            case MenuPage::Mirror:
                // 镜子页单击不退出，避免误触；双击、长按或返回退出。
                break;
            case MenuPage::Network:
                if (focus_ == 1)
                    return MenuAction::ChangeNetwork;
                if (focus_ == 2) {
                    Open(MenuPage::ConfirmBind);
                    break;
                }
                Open(MenuPage::Main);
                break;
            case MenuPage::ConfirmBind:
                if (focus_ == 1) {
                    Home();
                    return MenuAction::ProvisionBind;
                }
                Open(MenuPage::Network);
                break;
            case MenuPage::Power:
                if (focus_ == 0)
                    Open(MenuPage::Main);
                else
                    Open(focus_ == 1 ? MenuPage::ConfirmPower : MenuPage::ConfirmReset);
                break;
            case MenuPage::ConfirmPower:
            case MenuPage::ConfirmReset: {
                const auto action =
                    page_ == MenuPage::ConfirmPower ? MenuAction::Power : MenuAction::Reset;
                if (focus_ == 1) {
                    Home();
                    return action;
                }
                Open(MenuPage::Power);
                break;
            }
            default:
                Open(MenuPage::Main);
                break;
        }
        return MenuAction::None;
    }

    // SyncValues 同步硬件现值；仅在进入菜单之前调用，避免覆盖正在编辑的数值。
    void SyncValues(int volume, int brightness) {
        volume_ = Clamp(volume);
        brightness_ = Clamp(brightness);
    }
    // ForceHome 供板级独占界面（如通话页）收回后归位主页；只重置层级，不触发动作。
    void ForceHome() { Home(); }
    // RequestNetworkConfirmation 默认取消；旋转不能延长远端请求的 30 秒物理确认时限。
    void RequestNetworkConfirmation(uint32_t now) {
        Open(MenuPage::ConfirmNetwork);
        confirmation_started_ = now;
        touched_ = now;
    }
    // ShowNetworkStatus 接收已去重的事务快照；阶段 9 是待确认，不能覆盖物理确认页。
    void ShowNetworkStatus(uint8_t stage, bool blocked, uint32_t now) {
        network_saved_ = stage == 7 && !blocked;
        network_finishing_ = false;
        if (blocked || (stage >= 1 && stage <= 6))
            Open(MenuPage::NetworkProgress);
        else if (stage == 7 || stage == 8)
            Open(MenuPage::NetworkResult);
        else
            return;
        touched_ = now;
    }
    // Page、Focus、Volume、Brightness 为渲染层提供只读状态。
    MenuPage Page() const { return page_; }
    size_t Focus() const { return focus_; }
    int Volume() const { return volume_; }
    int Brightness() const { return brightness_; }
    // SyncSkin 同步实际主题，重新进入选择页时聚焦当前皮肤。
    void SyncSkin(size_t index) {
        const size_t selected = index < 3 ? index : 0;  // 非法配置回默认皮肤。
        if (selected != skin_index_) ++revision_;
        skin_index_ = selected;
    }
    size_t Skin() const { return skin_index_; }  // 应用任务使用本次选择的皮肤索引。
    // Count 返回当前页面可确认的条目数，危险确认页首项始终为取消。
    size_t Count() const {
        if (page_ == MenuPage::Main)
            return 7;
        if (page_ == MenuPage::Skins)
            return 4;
        if (page_ == MenuPage::Home)
            return 5;
        if (page_ == MenuPage::Contact)
            return 3;
        if (page_ == MenuPage::Contacts)
            return contacts_loading_ ? 1
                                     : 2 + contacts_count_ + (contacts_offset_ > 0 ? 1 : 0) +
                                           (contacts_more_ ? 1 : 0);
        if (page_ == MenuPage::Power)
            return 3;
        if (page_ == MenuPage::Network)
            return 3;
        if (page_ == MenuPage::Volume || page_ == MenuPage::Brightness)
            return 3;
        if (page_ == MenuPage::ConfirmBind)
            return 2;
        if (page_ == MenuPage::NetworkProgress)
            return 3;
        if (page_ == MenuPage::NetworkResult)
            return network_saved_ ? 1 : 2;
        if (page_ == MenuPage::Network || page_ == MenuPage::ConfirmPower ||
            page_ == MenuPage::ConfirmReset || page_ == MenuPage::ConfirmNetwork)
            return 2;
        return 1;
    }

    // ApplyContacts 只接纳原查询页面及分页位置；翻页、退出和旧抬手均不能绑定新列表。
    bool ApplyContacts(uint32_t revision, uint32_t offset, size_t count, bool more, bool ok) {
        if (page_ != MenuPage::Contacts || revision_ != revision || offset != contacts_offset_ ||
            count > 4)
            return false;
        contacts_loading_ = false;
        contacts_failed_ = !ok;
        contacts_count_ = ok ? count : 0;
        if (contacts_slice_ >= contacts_count_)
            contacts_slice_ = 0;
        contacts_more_ = ok && more;
        if (!ok)
            contacts_offset_ = 0;
        focus_ = ok && count ? contacts_slice_ + 2 : 1;
        ++revision_;
        return true;
    }
    // 通讯录只读状态供显示任务渲染和复制原拨号目标，不能通过名称反查替换目标。
    size_t ContactsCount() const { return contacts_count_; }
    size_t SelectedContact() const { return selected_contact_; }
    uint32_t ContactsOffset() const { return contacts_offset_; }
    bool ContactsLoading() const { return contacts_loading_; }
    bool ContactsFailed() const { return contacts_failed_; }
    bool ContactsMore() const { return contacts_more_; }

private:
    // BeginContacts 清除原页选择后发起查询，加载期间只有返回入口。
    MenuAction BeginContacts(uint32_t offset, size_t slice = 0) {
        Open(MenuPage::Contacts);
        contacts_offset_ = offset;
        contacts_count_ = 0;
        contacts_slice_ = slice;
        contacts_loading_ = true;
        contacts_failed_ = contacts_more_ = false;
        return MenuAction::LoadContacts;
    }
    // Clamp 限定可持久化设置的百分比范围。
    static int Clamp(int value) { return value < 0 ? 0 : value > 100 ? 100 : value; }
    // Open 进入新页面重置焦点，确保危险确认不会继承“确认”。
    void Open(MenuPage page) {
        page_ = page;
        focus_ = (page == MenuPage::Main || page == MenuPage::Contact) ? 1 : 0;
        ++revision_;
    }
    // Home 结束菜单展示，不直接操作会话或电源。
    void Home() {
        Open(MenuPage::Home);
        focus_ = home_focus_;
    }
    MenuPage page_ = MenuPage::Home;     // 当前层级。
    size_t skin_index_ = 0;              // 已选或待应用的皮肤，默认暗夜绿。
    size_t home_focus_ = 0;              // 表盘选中功能，返回时恢复。
    bool conversation_active_ = false;   // 原设备状态确认的AI连接、收音或播放阶段。
    size_t contacts_slice_ = 0;          // 批次内部两条一屏，返回详情时保留。
    size_t focus_ = 0;                   // 当前选中条目。
    uint32_t revision_ = 0;              // 页面代次，阻止触摸按下后发生翻页的迟到点击。
    uint32_t touched_ = 0;               // 最后一次用户操作的毫秒时间。
    uint32_t confirmation_started_ = 0;  // 云端请求首次显示时刻，独立于交互时间。
    int volume_ = 50;                    // 编辑中的音量百分比。
    int brightness_ = 50;                // 编辑中的亮度百分比。
    size_t contacts_count_ = 0;          // 当前页最多四项，所有联系人按需分页查询。
    size_t selected_contact_ = 0;        // 详情页选中的原列表索引。
    uint32_t contacts_offset_ = 0;       // 当前查询分页位置。
    bool contacts_loading_ = false;      // 加载期间禁止选中上页旧行。
    bool contacts_failed_ = false;       // 失败页允许返回或重新查询。
    bool contacts_more_ = false;         // 仅平台确认后显示下一页。
    bool network_saved_ = false;         // 成功页仅允许结束独占流程，不再取消已保存网络。
    bool network_finishing_ = false;     // 完成动作只发一次，忽略连续点击。
};

}  // namespace watcher
