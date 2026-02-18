#include <ydb/library/actors/interconnect/interconnect_tcp_session.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NActors {

Y_UNIT_TEST_SUITE(InputSessionUpdate) {
    Y_UNIT_TEST(NoDeltaWithoutPending) {
        UNIT_ASSERT(!HasInputSessionUpdateDelta(10, 10, 0, nullptr));
    }

    Y_UNIT_TEST(ConfirmDeltaWithoutPending) {
        UNIT_ASSERT(HasInputSessionUpdateDelta(11, 10, 0, nullptr));
    }

    Y_UNIT_TEST(DataDeltaWithoutPending) {
        UNIT_ASSERT(HasInputSessionUpdateDelta(10, 10, 1, nullptr));
    }

    Y_UNIT_TEST(PendingUpdateUsesPendingConfirmAsBaseline) {
        TEvUpdateFromInputSession pending(11, 0, TDuration::Zero());
        UNIT_ASSERT(!HasInputSessionUpdateDelta(11, 10, 0, &pending));
        UNIT_ASSERT(HasInputSessionUpdateDelta(12, 10, 0, &pending));
    }

    Y_UNIT_TEST(DataDeltaWithPending) {
        TEvUpdateFromInputSession pending(11, 0, TDuration::Zero());
        UNIT_ASSERT(HasInputSessionUpdateDelta(11, 10, 5, &pending));
    }
}

} // namespace NActors
