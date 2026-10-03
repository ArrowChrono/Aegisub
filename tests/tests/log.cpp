#include <libaegisub/log.h>
#include <main.h>
#include <algorithm>
#include <string>
#include <string_view>

namespace {
std::string LastMessage(char const *section) {
    auto messages = agi::log::log->GetMessages();
    auto found = std::find_if(messages.rbegin(), messages.rend(), [=](auto const& message) {
        return std::string_view(message.section) == section;
    });
    EXPECT_NE(found, messages.rend());
    return found == messages.rend() ? std::string() : found->message;
}
}

TEST(lagi_log, oversized_message_is_safely_truncated) {
    ASSERT_NE(nullptr, agi::log::log);
    LOG_W("tests/log/oversized") << std::string(8192, 'x');
    EXPECT_EQ(std::string(2048, 'x'), LastMessage("tests/log/oversized"));
}

TEST(lagi_log, exact_capacity_message_is_preserved) {
    ASSERT_NE(nullptr, agi::log::log);
    LOG_W("tests/log/exact") << std::string(2048, 'y');
    EXPECT_EQ(std::string(2048, 'y'), LastMessage("tests/log/exact"));
}

TEST(lagi_log, empty_message_is_preserved) {
    ASSERT_NE(nullptr, agi::log::log);
    LOG_W("tests/log/empty") << "";
    EXPECT_EQ("", LastMessage("tests/log/empty"));
}
