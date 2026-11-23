// 文件说明：纯名册正常、非法、资源边界与原子投影回归；不创建网络或媒体驱动。
#include "ur/device/participant_roster.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// member 生成仅成员的合法完整条目，不伪造媒体源。
std::string member(std::string id, const char* change = "joined") {
    return "{\"participantId\":\"" + id + "\",\"change\":\"" + change + "\",\"tracks\":[]}";
}
// track 构造公共完整Track供纯元数据校验，不调用驱动。
std::string track(const std::string& id) {
    return "{\"trackId\":\"" + id +
           "\",\"kind\":\"audio\",\"sourceId\":\"mic\",\"direction\":\"recvonly\",\"codec\":"
           "\"opus\",\"state\":\"active\"}";
}
TEST(ParticipantRoster, JoinReplaceLeaveAndClear) {
    ParticipantRoster roster;
    ASSERT_TRUE(roster.apply(member("101")));
    ASSERT_TRUE(roster.apply("{\"participantId\":\"101\",\"change\":\"joined\",\"tracks\":[" +
                             track("track-a") + "]}"));
    ASSERT_EQ(roster.members().size(), 1U);
    ASSERT_EQ(roster.members().front().tracks.size(), 1U);
    EXPECT_EQ(roster.members().front().tracks.front().codec, "opus");
    ASSERT_TRUE(roster.apply(member("101")));
    EXPECT_TRUE(roster.members().front().tracks.empty());
    ASSERT_TRUE(roster.apply(member("unknown", "left")));
    EXPECT_EQ(roster.members().size(), 1U);
    ASSERT_TRUE(roster.apply(member("101", "left")));
    EXPECT_TRUE(roster.members().empty());
    ASSERT_TRUE(roster.apply(member("102")));
    roster.clear();
    EXPECT_TRUE(roster.members().empty());
}
TEST(ParticipantRoster, InvalidBodyIsAtomic) {
    ParticipantRoster roster;
    ASSERT_TRUE(roster.apply(member("101")));
    for (const auto* body :
         {"", "null", "[]", "{}", R"({"participantId":"101","change":"snapshot","tracks":[]})",
          R"({"participantId":"101","change":"joined","tracks":null})",
          R"({"participantId":"101","change":"joined","tracks":[],"extra":1})",
          R"({"participantId":"101","participant\u0049d":"102","change":"joined","tracks":[]})",
          R"({"participantId":"101\n","change":"joined","tracks":[]})",
          R"({"participantId":"101\u0000hidden","change":"joined","tracks":[]})",
          R"({"participantId":1,"change":"joined","tracks":[]})",
          R"({"participantId":"101","change":"joined","tracks":[{}]})"}) {
        EXPECT_FALSE(roster.apply(body));
        ASSERT_EQ(roster.members().size(), 1U);
        EXPECT_EQ(roster.members().front().participant_id, "101");
    }
    EXPECT_FALSE(roster.apply(std::string(65537, ' ')));
    EXPECT_FALSE(roster.apply(member(std::string(129, 'a'))));
    EXPECT_TRUE(
        roster.apply(R"({"participantId":"literal\\u0000","change":"joined","tracks":[]})"));
}
TEST(ParticipantRoster, TrackSetStrictAndCrossMemberUnique) {
    ParticipantRoster roster;
    ASSERT_TRUE(roster.apply("{\"participantId\":\"101\",\"change\":\"joined\",\"tracks\":[" +
                             track("track-a") + "]}"));
    EXPECT_FALSE(roster.apply("{\"participantId\":\"102\",\"change\":\"joined\",\"tracks\":[" +
                              track("track-a") + "]}"));
    EXPECT_FALSE(roster.apply("{\"participantId\":\"101\",\"change\":\"joined\",\"tracks\":[" +
                              track("a") + "," + track("a") + "]}"));
    const auto valid = track("b");
    for (const auto* invalid : {"\"other\"", "null", "0"}) {
        std::string bad = valid;
        const auto at = bad.find("\"audio\"");
        bad.replace(at, 7, invalid);
        EXPECT_FALSE(roster.apply("{\"participantId\":\"102\",\"change\":\"joined\",\"tracks\":[" +
                                  bad + "]}"));
    }
    std::string tracks;
    for (int index = 0; index < 9; ++index) {
        if (index)
            tracks += ',';
        tracks += track("track-" + std::to_string(index));
    }
    EXPECT_FALSE(roster.apply("{\"participantId\":\"102\",\"change\":\"joined\",\"tracks\":[" +
                              tracks + "]}"));
    EXPECT_EQ(roster.members().size(), 1U);
}
TEST(ParticipantRoster, MemberCapacityDoesNotEvictOld) {
    ParticipantRoster roster;
    for (int index = 0; index < 32; ++index)
        ASSERT_TRUE(roster.apply(member(std::to_string(index))));
    EXPECT_FALSE(roster.apply(member("overflow")));
    EXPECT_EQ(roster.members().size(), 32U);
    EXPECT_TRUE(roster.apply(member("0")));
    EXPECT_TRUE(roster.apply(member("0", "left")));
    EXPECT_TRUE(roster.apply(member("replacement")));
    EXPECT_EQ(roster.members().size(), 32U);
}
// 逻辑预算失败保持原条目；不是把固定成员上限冒称实际堆内存限额。
TEST(ParticipantRoster, LogicalBudgetIsAtomic) {
    ParticipantRoster roster;
    bool refused = false;
    for (int index = 0; index < 32; ++index) {
        std::string tracks;
        for (int number = 0; number < 8; ++number) {
            if (number)
                tracks += ',';
            tracks += "{\"trackId\":\"" + std::to_string(index) + "-" + std::to_string(number) +
                      std::string(100, 't') + "\",\"kind\":\"video\",\"sourceId\":\"" +
                      std::string(128, 's') + "\",\"direction\":\"sendrecv\",\"codec\":\"" +
                      std::string(64, 'c') + "\",\"state\":\"active\"}";
        }
        const auto old = roster.members().size();
        const bool accepted = roster.apply("{\"participantId\":\"" + std::to_string(index) +
                                           "\",\"change\":\"joined\",\"tracks\":[" + tracks + "]}");
        if (!accepted) {
            EXPECT_EQ(roster.members().size(), old);
            refused = true;
            break;
        }
    }
    EXPECT_TRUE(refused);
    EXPECT_LT(roster.members().size(), 32U);
}

TEST(ParticipantRoster, SnapshotReplacesAtomicallyAndExplicitEmptyClears) {
    ParticipantRoster roster;
    ASSERT_TRUE(roster.apply(member("old")));
    ASSERT_TRUE(roster.replace(
        R"([{"participantId":"101","tracks":[]},{"participantId":"102","tracks":[]}])"));
    ASSERT_EQ(roster.members().size(), 2U);
    EXPECT_EQ(roster.members()[0].participant_id, "101");
    EXPECT_EQ(roster.members()[1].participant_id, "102");
    ASSERT_TRUE(roster.apply(member("103")));
    ASSERT_TRUE(roster.replace("[]"));
    EXPECT_TRUE(roster.members().empty());
}

TEST(ParticipantRoster, MalformedSnapshotPreservesWholePriorProjection) {
    ParticipantRoster roster;
    ASSERT_TRUE(roster.apply(member("old")));
    for (
        const auto* body :
        {"", "null", "{}", "[{}]", R"([{"participantId":"101","tracks":null}])",
         R"([{"participantId":"101","tracks":[],"change":"joined"}])",
         R"([{"participantId":"101","participant\u0049d":"102","tracks":[]}])",
         R"([{"participantId":"101","tracks":[]},{"participantId":"101","tracks":[]}])",
         R"([{"participantId":"101","tracks":[]},{"participantId":"102\u0000hidden","tracks":[]}])"}) {
        EXPECT_FALSE(roster.replace(body));
        ASSERT_EQ(roster.members().size(), 1U);
        EXPECT_EQ(roster.members().front().participant_id, "old");
    }
    EXPECT_FALSE(roster.replace(std::string(65537, ' ')));
}

TEST(ParticipantRoster, SnapshotCapacityAndCrossMemberTracksAreStrict) {
    ParticipantRoster roster;
    std::string full = "[";
    for (int index = 0; index < 32; ++index) {
        if (index)
            full += ',';
        full += "{\"participantId\":\"" + std::to_string(index) + "\",\"tracks\":[]}";
    }
    full += ']';
    ASSERT_TRUE(roster.replace(full));
    auto oversized = full;
    oversized.pop_back();
    oversized += R"(,{"participantId":"overflow","tracks":[]}])";
    EXPECT_FALSE(roster.replace(oversized));
    EXPECT_EQ(roster.members().size(), 32U);
    EXPECT_FALSE(roster.replace("[{\"participantId\":\"101\",\"tracks\":[" + track("same") +
                                "]},{\"participantId\":\"102\",\"tracks\":[" + track("same") +
                                "]}]"));
    EXPECT_EQ(roster.members().size(), 32U);
}
TEST(ParticipantRoster, DirectEntriesRejectMalformedUtf8WithoutMutatingProjection) {
    ParticipantRoster roster;
    ASSERT_TRUE(roster.apply(member("old")));
    for (const auto& bytes : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                              std::string("\xf4\x90\x80\x80", 4), std::string("\xc2", 1)}) {
        EXPECT_FALSE(roster.apply(member(bytes)));
        EXPECT_FALSE(roster.replace("[{\"participantId\":\"" + bytes + "\",\"tracks\":[]}]"));
        ASSERT_EQ(roster.members().size(), 1U);
        EXPECT_EQ(roster.members().front().participant_id, "old");
    }
    EXPECT_TRUE(roster.replace(R"([{"participantId":"用户甲","tracks":[]}])"));
}
}  // namespace
}  // namespace ur::device
