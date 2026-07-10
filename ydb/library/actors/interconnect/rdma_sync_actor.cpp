
#include "events_local.h"
#include "rdma_sync_actor.h"
#include "interconnect_tcp_session.h"
#include "interconnect_tcp_proxy.h"



#include <ydb/library/actors/core/actor_coroutine.h>
#include <ydb/library/actors/core/actor.h>

#include <variant>

namespace NInterconnect::NRdma {

namespace {
    using namespace NActors;
    static constexpr ui32 StackSize = 64 * 1024;

    class TSessionCreatorDelegate final : public NActors::TEvPrepareRdmaHandshake {
    public:
        TSessionCreatorDelegate(
                TActorId synActor,
                NInterconnect::NRdma::TQueuePair::TPtr qp,
                NInterconnect::NRdma::ICq::TPtr cq)
            : SyncActor(synActor)
            , Qp(std::move(qp))
            , Cq(std::move(cq))
        {}

        const TString* GetError() const noexcept {
            return std::get_if<TString>(&Res);
        }

    private:
        // calling from proxy context
        void virtual CreateSession(TInterconnectProxyTCP* const proxy) override {
            Y_DEBUG_ABORT_UNLESS(std::holds_alternative<std::monostate>(Res));
            auto session = new TInterconnectSessionRdma(proxy, Qp);
            TlsActivationContext->AsActorContext().RegisterWithSameMailbox(session);
            IActor::InvokeOtherActor(*session, &TInterconnectSessionRdma::ToSyncMode, SyncActor, Cq);
            Res = session;
        }
        void virtual ReportError(TString error) override {
            Y_DEBUG_ABORT_UNLESS(std::holds_alternative<std::monostate>(Res));
            Res = std::move(error);
        }
        const TActorId SyncActor;
        NInterconnect::NRdma::TQueuePair::TPtr Qp;
        NInterconnect::NRdma::ICq::TPtr Cq;

        std::variant<std::monostate, NActors::TInterconnectSessionRdma*, TString> Res;
    };

    class TRdmaSyncActor
       : public NActors::TActorCoroImpl
       , public NActors::TInterconnectLoggingBase
    {
        NActors::TInterconnectProxyCommon::TPtr Common;
        const ui32 PeerNodeId;
        TIntrusivePtr<NInterconnect::TStreamSocket> Socket;
        TQueuePair::TPtr Qp;
        ICq::TPtr Cq;
        const bool Incoming;
        TActorId Creator;

    public:
        TRdmaSyncActor(
                NActors::TInterconnectProxyCommon::TPtr common,
                ui32 peerNodeId,
                TIntrusivePtr<NInterconnect::TStreamSocket> socket,
                TQueuePair::TPtr qp,
                ICq::TPtr cq,
                bool incoming)
            : TActorCoroImpl(NActors::UsePooledStack<StackSize>(), true)
            , Common(std::move(common))
            , PeerNodeId(peerNodeId)
            , Socket(std::move(socket))
            , Qp(std::move(qp))
            , Cq(std::move(cq))
            , Incoming(incoming)
        {
            Creator = TlsActivationContext->AsActorContext().SelfID;
        }

        void Run() override {
            SetPrefix(Sprintf("RdmaSync %s [node %" PRIu32 " %s]",
                SelfActorId.ToString().data(), PeerNodeId, Incoming ? "incoming" : "outgoing"));
            LOG_LOG_IC_X(NActorsServices::INTERCONNECT, "ICRDMA", NActors::NLog::PRI_DEBUG,
                "starting rdma sync actor");

            Send(GetActorSystem()->InterconnectProxy(PeerNodeId),
                new TSessionCreatorDelegate(SelfActorId, Qp, Cq));
            auto ev = TActorCoroImpl::WaitForEvent();
            auto* result = static_cast<TSessionCreatorDelegate*>(ev->GetBase());
            if (const TString* error = result->GetError()) {
                LOG_LOG_IC_X(NActorsServices::INTERCONNECT, "ICRDMA", NActors::NLog::PRI_ERROR,
                    "unable to create rdma sync session: %s", error->data());
                Send(Creator, new TEvRdmaSyncResult(*error));
                return;
            }

            Y_UNUSED(Common);
            Y_UNUSED(Socket);

        }
    };
}

NActors::IActor* CreateRdmaOutgoingSyncActor(
    NActors::TInterconnectProxyCommon::TPtr common,
    ui32 peerNodeId,
    TIntrusivePtr<NInterconnect::TStreamSocket> socket,
    TQueuePair::TPtr qp,
    ICq::TPtr cq)
{
    return new NActors::TActorCoro(MakeHolder<TRdmaSyncActor>(
        std::move(common), peerNodeId, std::move(socket), std::move(qp), std::move(cq), false));
}

NActors::IActor* CreateRdmaIncommingSyncActor(
    NActors::TInterconnectProxyCommon::TPtr common,
    ui32 peerNodeId,
    TIntrusivePtr<NInterconnect::TStreamSocket> socket,
    TQueuePair::TPtr qp,
    ICq::TPtr cq)
{
    return new NActors::TActorCoro(MakeHolder<TRdmaSyncActor>(
        std::move(common), peerNodeId, std::move(socket), std::move(qp), std::move(cq), true));
}

}
