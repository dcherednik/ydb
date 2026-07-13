#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/interconnect/events_local.h>
#include <ydb/library/actors/interconnect/interconnect_stream.h>
#include <ydb/library/actors/interconnect/rdma/ctx.h>
#include <ydb/library/actors/interconnect/rdma/link_manager.h>
#include <ydb/library/actors/interconnect/rdma/mem_pool.h>
#include <ydb/library/actors/interconnect/rdma/rdma.h>
#include <ydb/library/actors/interconnect/rdma_sync_actor.h>
#include <ydb/library/actors/interconnect/rdma/ut/utils/utils.h>
#include <ydb/library/actors/interconnect/ut/lib/ic_test_cluster.h>

#include <library/cpp/testing/gtest/gtest.h>
#include <ydb/library/testlib/unittest_gtest_macro_subst.h>

#include <util/network/pair.h>
#include <util/network/socket.h>
#include <util/system/env.h>

using namespace NActors;
using namespace NInterconnect::NRdma;

namespace {

static void GTestSkip() {
    GTEST_SKIP() << "Skipping all rdma tests for suite, set \""
                 << NRdmaTest::RdmaTestEnvSwitchName << "\" env if it is RDMA compatible";
}

class TRdmaSyncActorTest
    : public ::testing::TestWithParam<ECqMode>
{
public:
    void SetUp() override {
        if (NRdmaTest::IsRdmaTestDisabled()) {
            GTestSkip();
        }
    }
};

static TRdmaCtx* GetTestRdmaCtx() {
    const TString ip = GetEnv("IP_TO_BIND_RDMA_TEST") ?: "::1";
    NInterconnect::TAddress address(ip, 7777);
    return NLinkMgr::GetCtx(address);
}

static void ConnectQps(TRdmaCtx* ctx, TQueuePair& qp1, TQueuePair& qp2) {
    ASSERT_EQ(qp1.ToRtsState(THandshakeData{
        .QpNum = qp2.GetQpNum(),
        .SubnetPrefix = ctx->GetGid().global.subnet_prefix,
        .InterfaceId = ctx->GetGid().global.interface_id,
        .MtuIndex = ctx->GetPortAttr().active_mtu,
    }), 0);

    ASSERT_EQ(qp2.ToRtsState(THandshakeData{
        .QpNum = qp1.GetQpNum(),
        .SubnetPrefix = ctx->GetGid().global.subnet_prefix,
        .InterfaceId = ctx->GetGid().global.interface_id,
        .MtuIndex = ctx->GetPortAttr().active_mtu,
    }), 0);
}

static ICq::TPtr CreateTestCq(TRdmaCtx* ctx, TActorSystem* actorSystem, std::shared_ptr<IMemPool> memPool, ECqMode mode) {
    const TRdmaRuntimeParams params{
        .MaxCqe = 32,
        .MaxWr = 16,
        .MaxSrqWr = 16,
        .RecieveBufSz = 4096,
    };

    switch (mode) {
        case ECqMode::POLLING:
            return CreateSimpleCq(ctx, actorSystem, params, std::move(memPool), nullptr);
        case ECqMode::EVENT:
            return CreateSimpleEventDrivenCq(ctx, actorSystem, params, std::move(memPool), nullptr);
    }
    Y_ABORT("unexpected CQ mode");
}

static std::pair<TIntrusivePtr<NInterconnect::TStreamSocket>, TIntrusivePtr<NInterconnect::TStreamSocket>> CreateSocketPair() {
    SOCKET fds[2];
    SocketPair(fds);
    SetNonBlock(fds[0]);
    SetNonBlock(fds[1]);
    return {
        MakeIntrusive<NInterconnect::TStreamSocket>(fds[0]),
        MakeIntrusive<NInterconnect::TStreamSocket>(fds[1]),
    };
}

static TInterconnectProxyCommon::TPtr CreateSyncCommon() {
    auto common = MakeIntrusive<TInterconnectProxyCommon>();
    common->MonCounters = MakeIntrusive<NMonitoring::TDynamicCounters>();
    common->RdmaMemPool = CreateSlotMemPool(nullptr, {});
    common->Settings.EnableRdmaSendReceive = true;
    return common;
}

class TRunRdmaSyncActor : public TActorBootstrapped<TRunRdmaSyncActor> {
public:
    TRunRdmaSyncActor(
            TInterconnectProxyCommon::TPtr common,
            TActorId selfVirtualId,
            TActorId peerVirtualId,
            ui32 peerNodeId,
            TIntrusivePtr<NInterconnect::TStreamSocket> socket,
            TQueuePair::TPtr qp,
            ICq::TPtr cq,
            bool incoming,
            NThreading::TPromise<TString> promise)
        : Common(std::move(common))
        , SelfVirtualId(selfVirtualId)
        , PeerVirtualId(peerVirtualId)
        , PeerNodeId(peerNodeId)
        , Socket(std::move(socket))
        , Qp(std::move(qp))
        , Cq(std::move(cq))
        , Incoming(incoming)
        , Promise(std::move(promise))
    {}

    void Bootstrap() {
        if (Incoming) {
            Register(CreateRdmaIncommingSyncActor(
                Common, SelfVirtualId, PeerVirtualId, PeerNodeId, std::move(Socket), std::move(Qp), std::move(Cq)));
        } else {
            Register(CreateRdmaOutgoingSyncActor(
                Common, SelfVirtualId, PeerVirtualId, PeerNodeId, std::move(Socket), std::move(Qp), std::move(Cq)));
        }
        Become(&TRunRdmaSyncActor::StateFunc);
    }

private:
    void Handle(TEvRdmaSyncResult::TPtr& ev) {
        Promise.SetValue(ev->Get()->Error);
        PassAway();
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvRdmaSyncResult, Handle);
    )

private:
    TInterconnectProxyCommon::TPtr Common;
    TActorId SelfVirtualId;
    TActorId PeerVirtualId;
    ui32 PeerNodeId = 0;
    TIntrusivePtr<NInterconnect::TStreamSocket> Socket;
    TQueuePair::TPtr Qp;
    ICq::TPtr Cq;
    bool Incoming = false;
    NThreading::TPromise<TString> Promise;
};

} // namespace

TEST_P(TRdmaSyncActorTest, CompletesSyncProtocol) {
    TRdmaCtx* ctx = GetTestRdmaCtx();
    ASSERT_TRUE(ctx);

    TTestICCluster cluster(2);

    auto common1 = CreateSyncCommon();
    auto common2 = CreateSyncCommon();

    auto cq1 = CreateTestCq(ctx, cluster.GetNode(1)->GetActorSystem(), common1->RdmaMemPool, GetParam());
    auto cq2 = CreateTestCq(ctx, cluster.GetNode(2)->GetActorSystem(), common2->RdmaMemPool, GetParam());
    ASSERT_TRUE(cq1);
    ASSERT_TRUE(cq2);
    ASSERT_TRUE(cq1->GetSrq());
    ASSERT_TRUE(cq2->GetSrq());

    auto qp1 = std::make_shared<TQueuePair>();
    auto qp2 = std::make_shared<TQueuePair>();
    ASSERT_EQ(qp1->Init(ctx, cq1.get(), 16), 0);
    ASSERT_EQ(qp2->Init(ctx, cq2.get(), 16), 0);
    ConnectQps(ctx, *qp1, *qp2);

    auto [socket1, socket2] = CreateSocketPair();

    const TActorId virtualId1(1, 0, 1, 1);
    const TActorId virtualId2(2, 0, 1, 2);

    auto promise1 = NThreading::NewPromise<TString>();
    auto future1 = promise1.GetFuture();
    auto promise2 = NThreading::NewPromise<TString>();
    auto future2 = promise2.GetFuture();

    cluster.RegisterActor(new TRunRdmaSyncActor(
        common1, virtualId1, virtualId2, 2, std::move(socket1), qp1, cq1, false, std::move(promise1)), 1);
    cluster.RegisterActor(new TRunRdmaSyncActor(
        common2, virtualId2, virtualId1, 1, std::move(socket2), qp2, cq2, true, std::move(promise2)), 2);

    const bool done1 = future1.Wait(TDuration::Seconds(30));
    const bool done2 = future2.Wait(TDuration::Seconds(30));

    cluster.StopNode(1);
    cluster.StopNode(2);
    qp1.reset();
    qp2.reset();
    cq1.reset();
    cq2.reset();

    ASSERT_TRUE(done1);
    ASSERT_TRUE(done2);

    const TString error1 = future1.GetValueSync();
    const TString error2 = future2.GetValueSync();
    EXPECT_TRUE(error1.empty()) << error1;
    EXPECT_TRUE(error2.empty()) << error2;
}

INSTANTIATE_TEST_SUITE_P(
    CqMode,
    TRdmaSyncActorTest,
    ::testing::Values(
        ECqMode::POLLING,
        ECqMode::EVENT
    ),
    [](const testing::TestParamInfo<ECqMode>& info) {
        switch (info.param) {
            case ECqMode::POLLING:
                return "POLLING";
            case ECqMode::EVENT:
                return "EVENT";
        }
        Y_ABORT("unexpected CQ mode");
    }
);
