// Trace ALL sd_bus_* calls made during sdbusplus::bus::match_t creation
// to determine the exact EXPECT_CALL sequence needed for StrictMock

#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::Return;

// Use NiceMock but log every call
class TraceMatchTest : public testing::Test
{
  protected:
    testing::NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);

    void SetUp() override
    {
        // Override NiceMock defaults with logging versions
        ON_CALL(mock, sd_bus_add_match(_, _, _, _, _))
            .WillByDefault([](sd_bus*, sd_bus_slot** slot, const char* match,
                              sd_bus_message_handler_t, void*) {
                printf("CALL: sd_bus_add_match(slot, match='%.40s...')\n",
                       match);
                *slot = reinterpret_cast<sd_bus_slot*>(0xdefa);
                return 0;
            });

        ON_CALL(mock, sd_bus_slot_set_destroy_callback(_, _))
            .WillByDefault([](sd_bus_slot* slot, sd_bus_destroy_t) {
                printf("CALL: sd_bus_slot_set_destroy_callback(slot=%p)\n",
                       (void*)slot);
                return 0;
            });

        ON_CALL(mock, sd_bus_slot_set_userdata(_, _))
            .WillByDefault([](sd_bus_slot* slot, void* ud) -> void* {
                printf("CALL: sd_bus_slot_set_userdata(slot=%p, ud=%p)\n",
                       (void*)slot, ud);
                return nullptr;
            });

        ON_CALL(mock, sd_bus_slot_unref(_))
            .WillByDefault([](sd_bus_slot* slot) -> sd_bus_slot* {
                printf("CALL: sd_bus_slot_unref(slot=%p)\n", (void*)slot);
                return nullptr;
            });

        ON_CALL(mock, sd_bus_message_new_method_call(_, _, _, _, _, _))
            .WillByDefault([](sd_bus*, sd_bus_message**, const char* dest,
                              const char* path, const char* iface,
                              const char* member) {
                printf(
                    "CALL: new_method_call(dest='%s', path='%s', iface='%s', method='%s')\n",
                    dest, path, iface, member);
                return 0;
            });

        ON_CALL(mock, sd_bus_call(_, _, _, _, _))
            .WillByDefault([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                              sd_bus_message** reply) {
                printf("CALL: sd_bus_call(reply=%p)\n",
                       reply ? (void*)*reply : nullptr);
                if (reply)
                    *reply = nullptr;
                return 0;
            });

        ON_CALL(mock, sd_bus_message_append_basic(_, _, _))
            .WillByDefault([](sd_bus_message*, char type, const void*) {
                printf("CALL: append_basic(type='%c')\n", type);
                return 0;
            });

        ON_CALL(mock, sd_bus_message_read_basic(_, _, _))
            .WillByDefault([](sd_bus_message*, char type, void*) {
                printf("CALL: read_basic(type='%c')\n", type);
                return 0;
            });

        ON_CALL(mock, sd_bus_message_enter_container(_, _, _))
            .WillByDefault([](sd_bus_message*, char type, const char* sig) {
                printf("CALL: enter_container(type='%c', sig='%s')\n", type,
                       sig ? sig : "null");
                return 0;
            });

        ON_CALL(mock, sd_bus_message_exit_container(_))
            .WillByDefault([](sd_bus_message*) {
                printf("CALL: exit_container()\n");
                return 0;
            });

        ON_CALL(mock, sd_bus_message_at_end(_, _))
            .WillByDefault([](sd_bus_message*, int) {
                printf("CALL: at_end()\n");
                return 1; // always at end
            });

        ON_CALL(mock, sd_bus_message_verify_type(_, _, _))
            .WillByDefault([](sd_bus_message*, char type, const char* sig) {
                printf("CALL: verify_type(type='%c', sig='%s')\n", type,
                       sig ? sig : "null");
                return 1;
            });

        ON_CALL(mock, sd_bus_message_open_container(_, _, _))
            .WillByDefault([](sd_bus_message*, char type, const char* sig) {
                printf("CALL: open_container(type='%c', sig='%s')\n", type,
                       sig ? sig : "null");
                return 0;
            });

        ON_CALL(mock, sd_bus_message_close_container(_))
            .WillByDefault([](sd_bus_message*) {
                printf("CALL: close_container()\n");
                return 0;
            });
    }
};

TEST_F(TraceMatchTest, TraceMatchCreation)
{
    printf("\n=== Creating match_t ===\n");
    auto matchRule = std::string("type='signal',interface='org.test'");
    {
        auto match = std::make_unique<sdbusplus::bus::match_t>(
            bus, matchRule, [](sdbusplus::message::message&) {});
        printf("\n=== Match created, now destroying ===\n");
    }
    printf("=== Match destroyed ===\n\n");
}
