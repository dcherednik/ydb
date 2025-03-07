
#include <ydb/library/actors/interconnect/interconnect_tcp_proxy.h>
#include <ydb/library/actors/interconnect/ut/protos/interconnect_test.pb.h>
#include <ydb/library/actors/interconnect/ut/lib/ic_test_cluster.h>
#include <ydb/library/actors/interconnect/ut/lib/interrupter.h>
#include <ydb/library/actors/interconnect/ut/lib/test_events.h>
#include <ydb/library/actors/interconnect/ut/lib/test_actors.h>
#include <ydb/library/actors/interconnect/ut/lib/node.h>

#include <library/cpp/testing/unittest/tests_data.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/network/sock.h>
#include <util/network/poller.h>
#include <library/cpp/deprecated/atomic/atomic.h>
#include <util/generic/set.h>

#include "zc_local_linux_mock/lib.h"

static void WaitAndCompare(std::atomic<ui64>& expected, std::atomic<ui64>& got) {
    while (expected.load(std::memory_order_relaxed) == 0) {
        NanoSleep(500ULL * 1000 * 1000);
    }
    const ui64 e = expected.load(std::memory_order_relaxed);
    i32 attempt = 30;
    while ((got.load(std::memory_order_relaxed) != e) && attempt--) {
        NanoSleep(500ULL * 1000 * 1000);
    }
    UNIT_ASSERT_VALUES_EQUAL(got.load(std::memory_order_relaxed), e);
}

Y_UNIT_TEST_SUITE(InterconnectZeroCopySendOp) {
    using namespace NActors;

    class TSenderActor: public TSenderBaseActor {
        TDeque<ui64> InFly;
        ui16 SendFlags;
        std::atomic<ui64>& Counter;
        ui64 Undelivered;

        ui64 Iterations;

    public:
        TSenderActor(const TActorId& recipientActorId, ui16 sendFlags, std::atomic<ui64>& c, ui64 iterations)
            : TSenderBaseActor(recipientActorId, 32)
            , SendFlags(sendFlags)
            , Counter(c)
            , Undelivered(0)
            , Iterations(iterations)
        {
        }

        ~TSenderActor() override {
            Cerr << "Sent " << SequenceNumber << " messages " << Undelivered << " \n";
            Cerr << "Sent " << Counter.load() << " messages\n";
        }

        void SendMessage(const TActorContext& ctx) override {
            const ui32 flags = IEventHandle::MakeFlags(0, SendFlags);
            const ui64 cookie = SequenceNumber;
            const TString payload(RandomNumber<size_t>(65536) + 4096, '@');
            auto ev = new TEvTest(SequenceNumber, payload);
            //Cerr << "SendMessage sz: " << payload.size() << Endl;
            //ev->Record.AddPayloadId(ev->AddPayload(TRope(payload)));
            ctx.Send(RecipientActorId, ev, flags, cookie);
            InFly.push_back(SequenceNumber);
            ++InFlySize;
            ++SequenceNumber;
        }

        void Handle(TEvents::TEvUndelivered::TPtr& ev, const TActorContext& ctx) override {
            auto record = std::find(InFly.begin(), InFly.end(), ev->Cookie);
            if (SendFlags & IEventHandle::FlagGenerateUnsureUndelivered) {
                if (record != InFly.end()) {
                    InFly.erase(record);
                    --InFlySize;
                    SendMessage(ctx);
                    Undelivered++;
                }
            } else {
                Y_ABORT_UNLESS(record != InFly.end());
            }
        }

        void Handle(TEvTestResponse::TPtr& ev, const TActorContext& ctx) override {
            Y_ABORT_UNLESS(InFly);
            const NInterconnectTest::TEvTestResponse& record = ev->Get()->Record;
            Y_ABORT_UNLESS(record.HasConfirmedSequenceNumber());
            if (!(SendFlags & IEventHandle::FlagGenerateUnsureUndelivered)) {
                while (record.GetConfirmedSequenceNumber() != InFly.front()) {
                    InFly.pop_front();
                    --InFlySize;
                }
            }
            Y_ABORT_UNLESS(record.GetConfirmedSequenceNumber() == InFly.front(), "got# %" PRIu64 " expected# %" PRIu64,
                     record.GetConfirmedSequenceNumber(), InFly.front());
            InFly.pop_front();
            --InFlySize;
            if (Iterations > 0) {
                Iterations--; 
                SendMessagesIfPossible(ctx);
            } else if (InFlySize == 0) {
                Finish();
                //TActivationContext::Schedule(TDuration::Seconds(1), new IEventHandle(SelfId(), SelfId(), new TEvents::TEvWakeup())); 
            }
        }

        void Finish() {
            Counter.store(SequenceNumber - Undelivered);
        }
    };

    class TReceiverActor: public TReceiverBaseActor {
        TNode* SenderNode = nullptr;
        std::atomic<ui64>& ReceivedCount;

    public:
        TReceiverActor(TNode* senderNode, std::atomic<ui64>& c)
            : TReceiverBaseActor()
            , SenderNode(senderNode)
            , ReceivedCount(c) 
        {
        }

        void Handle(TEvTest::TPtr& ev, const TActorContext& /*ctx*/) override {
            const NInterconnectTest::TEvTest& m = ev->Get()->Record;
            Y_ABORT_UNLESS(m.HasSequenceNumber());
            Y_ABORT_UNLESS(m.GetSequenceNumber() >= ReceivedCount, "got #%" PRIu64 " expected at least #%" PRIu64,
                     m.GetSequenceNumber(), ReceivedCount.load(std::memory_order_relaxed));
            ReceivedCount.fetch_add(1);
            SenderNode->Send(ev->Sender, new TEvTestResponse(m.GetSequenceNumber()));
        }

        ~TReceiverActor() override {
            Cerr << "Received " << ReceivedCount.load(std::memory_order_relaxed) << " messages\n";
        }
    };

   Y_UNIT_TEST(SimpleSend) {
        ui32 numNodes = 2;
        ui16 flags = IEventHandle::FlagTrackDelivery;

        std::atomic<ui64> s = 0;
        std::atomic<ui64> r = 0;
        {
            TTestICCluster testCluster(numNodes, TChannelsConfig());
            TReceiverActor* receiverActor = new TReceiverActor(testCluster.GetNode(1), r);
            const TActorId recipient = testCluster.RegisterActor(receiverActor, 2);
            TSenderActor* senderActor = new TSenderActor(recipient, flags, s, 1000);
            testCluster.RegisterActor(senderActor, 1);

            WaitAndCompare(s, r);

        }
        Cerr << s.load() << " .  " << r.load() << Endl;
        //UNIT_ASSERT_VALUES_EQUAL(s, r);
    }

    Y_UNIT_TEST(ReestablishSessionSend) {
        ui32 numNodes = 2;
        double bandWidth = 1000000;
        ui16 flags = IEventHandle::FlagTrackDelivery |  IEventHandle::FlagGenerateUnsureUndelivered;


        std::atomic<ui64> s = 0;
        std::atomic<ui64> r = 0;
        {
            TTestICCluster::TTrafficInterrupterSettings interrupterSettings{TDuration(), bandWidth, true};
            TTestICCluster testCluster(numNodes, TChannelsConfig(), &interrupterSettings);
            TReceiverActor* receiverActor = new TReceiverActor(testCluster.GetNode(1), r);
            const TActorId recipient = testCluster.RegisterActor(receiverActor, 2);
            TSenderActor* senderActor = new TSenderActor(recipient, flags, s, 200);
            testCluster.RegisterActor(senderActor, 1);

            WaitAndCompare(s, r);
        }
        Cerr << s.load() << " .  " << r.load() << Endl;
//        UNIT_ASSERT_VALUES_EQUAL(s, r);
    }
}
