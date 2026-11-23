"""验证等待期终态保留、时间回拨及容量耗尽时的安全边界。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PendingTombstonesTest(unittest.TestCase):
    def test_terminal_retention_and_capacity(self):
        source = r'''
#include "ur_signaling_pending_tombstones.h"
#include <cassert>
int main() {
 using Decision = PendingCallTombstones::Decision;
 PendingCallTombstones ledger;
 ledger.Advance(100);
 assert(ledger.CanReserve(10));
 assert(!ledger.CanReserve(65537));
 assert(!ledger.CanRecordOther(65536, 1));
 assert(ledger.Remember(std::string("t1"), std::string("s1"), std::string("p"), 200, Decision::Cancelled));
 assert(ledger.Find("t1", "s1", "changed")==Decision::Conflict);
 assert(ledger.Find("t1", "other", "p")==Decision::Conflict);
 assert(ledger.Find("other", "s1", "p")==Decision::Conflict);
 assert(ledger.Find("t2", "s2", "p")==Decision::None);
 ledger.Advance(99); assert(ledger.Find("t1", "s1", "p")==Decision::Cancelled);
 assert(ledger.CanRecordOther(10, 10));
 assert(ledger.Remember(std::string("t2"), std::string("s2"), std::string("p"), 210, Decision::Busy));
 assert(ledger.Remember(std::string("t3"), std::string("s3"), std::string("p"), 220, Decision::Cancelled));
 assert(ledger.Remember(std::string("t4"), std::string("s4"), std::string("p"), 230, Decision::Cancelled));
 assert(!ledger.CanReserve(10));
 assert(!ledger.CanRecordOther(10, 10));
 assert(!ledger.Remember(std::string("t5"), std::string("s5"), std::string("p"), 240, Decision::Cancelled));
 ledger.Fence(240);
 ledger.Advance(205); assert(ledger.Find("t1", "s1", "p")==Decision::None);
 assert(!ledger.CanReserve(10));
 ledger.Advance(240); assert(ledger.CanReserve(10));
 assert(ledger.Find("t4", "s4", "p")==Decision::None);
 assert(ledger.Expired(240));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'ledger')
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/signaling'), '-', '-o', binary],
                           input=source, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
