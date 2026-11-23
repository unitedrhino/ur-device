"""验证坦克电容按键在不同通话阶段不会执行错误动作。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class SparkBotCallControlsTest(unittest.TestCase):
    """直接编译生产映射，覆盖接听、拒接、挂断及空闲误触边界。"""

    def test_touch_actions_follow_call_state(self):
        root = Path(__file__).resolve().parents[2]
        code = r'''
#include <initializer_list>
#include "sparkbot_call_controls.h"
#include <cassert>
using namespace sparkbot;
int main() {
    assert(ResolveCallAction(CallState::Ringing, TouchSide::Left) == CallAction::Accept);
    assert(ResolveCallAction(CallState::Ringing, TouchSide::Right) == CallAction::Reject);
    for (auto state : {CallState::Connecting, CallState::Active}) {
        assert(ResolveCallAction(state, TouchSide::Right) == CallAction::HangUp);
        assert(ResolveCallAction(state, TouchSide::Left) == CallAction::None);
    }
    for (auto state : {CallState::Hidden, CallState::Ended}) {
        assert(ResolveCallAction(state, TouchSide::Left) == CallAction::None);
        assert(ResolveCallAction(state, TouchSide::Right) == CallAction::None);
    }
    CallStateEpoch<CallState> epochs(CallState::Hidden);
    epochs.Store(CallState::Ringing, 17);
    auto old_ringing = epochs.Snapshot();
    assert(old_ringing.generation == 17);
    epochs.Store(CallState::Hidden, 18);
    epochs.Store(CallState::Ringing, 18);
    assert(old_ringing.generation == 17 && epochs.Snapshot().generation == 18);
    assert(!epochs.IsCurrent(old_ringing) && epochs.IsCurrent(epochs.Snapshot()));
    epochs.Store(CallState::Hidden, 18);
    auto before_call = epochs.Snapshot();
    assert(epochs.AllowsChat(before_call, CallState::Hidden));
    epochs.Store(CallState::Ringing);
    assert(!epochs.AllowsChat(before_call, CallState::Hidden));
    auto ringing = epochs.Snapshot();
    assert(!epochs.AllowsChat(ringing, CallState::Hidden));
    epochs.Store(CallState::Hidden);
    assert(!epochs.AllowsChat(before_call, CallState::Hidden));
    assert(epochs.AllowsChat(epochs.Snapshot(), CallState::Hidden));
    CallTouchGate same_state;
    assert(same_state.Update(CallState::Ringing, 0, false, false, 17) == CallAction::None);
    assert(same_state.Update(CallState::Ringing, 1U << 2, true, false, 18) == CallAction::None);
    assert(same_state.Update(CallState::Ringing, 0, false, false, 18) == CallAction::None);
    assert(same_state.Update(CallState::Ringing, 1U << 2, true, false, 18) == CallAction::Accept);
    CallTouchGate gate;
    assert(gate.Update(CallState::Ringing, 1U << 2, false, false) == CallAction::None);
    assert(gate.Update(CallState::Ringing, 1U << 2, true, false) == CallAction::None);
    assert(gate.Update(CallState::Ringing, 0, false, false) == CallAction::None);
    assert(gate.Update(CallState::Ringing, 1U << 2, true, false) == CallAction::Accept);
    assert(gate.Update(CallState::Ringing, (1U << 2) | (1U << 3), true, true) == CallAction::None);
    assert(gate.Update(CallState::Ringing, (1U << 2) | (1U << 3), true, false) == CallAction::None);
    assert(gate.Update(CallState::Active, 1U << 3, false, true) == CallAction::None);
    assert(gate.Update(CallState::Active, 0, false, false) == CallAction::None);
    assert(gate.Update(CallState::Active, 1U << 3, false, true) == CallAction::HangUp);
    assert(gate.Update(CallState::Hidden, 1U << 3, false, false) == CallAction::None);
    assert(!gate.AllowChat());
    gate.Update(CallState::Hidden, 0, false, false);
    assert(gate.AllowChat());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'test.cc'
            source.write_text(code)
            binary = Path(directory) / 'test'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(root / 'main/boards/espressif/esp-sparkbot'),
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
