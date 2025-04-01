#include "interconnect_zc_processor.h"

#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/core/hfunc.h>

#if defined (__linux__)

#define YDB_MSG_ZEROCOPY_SUPPORTED 1

#endif

#ifdef YDB_MSG_ZEROCOPY_SUPPORTED

#include <linux/errqueue.h>
#include <linux/netlink.h>

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

// Whether the cmsg received from error queue is of the IPv4 or IPv6 levels.
static bool CmsgIsIpLevel(const cmsghdr& cmsg) {
    return (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR) ||
       (cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR);
}

static bool CmsgIsZeroCopy(const cmsghdr& cmsg) {
    if (!CmsgIsIpLevel(cmsg)) {
        return false;
    }
    auto serr = reinterpret_cast<const sock_extended_err*> CMSG_DATA(&cmsg);
    return serr->ee_errno == 0 && serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY;
}

#endif

namespace NInterconnect {
using NActors::TEvents;


TInterconnectZcProcessor*  TInterconnectZcProcessor::Register(const NActors::TActorContext &ctx, bool enabled) {
    TInterconnectZcProcessor* actor(new TInterconnectZcProcessor(enabled));
    // must be registered on the same mailbox!
    ctx.RegisterWithSameMailbox(actor);
    return actor;
}

STFUNC(TInterconnectZcProcessor::StateFunc) {
    STRICT_STFUNC_BODY(
        cFunc(TEvents::TEvPoisonPill::EventType, HandlePoison)
    )
}

void TInterconnectZcProcessor::ScheduleTermination(std::unique_ptr<NActors::TEventHolderPool>&& pool) {
    Pool = std::move(pool);
    Cerr << "ScheduleTermination: " << Delayed.size() << Endl;
}

void TInterconnectZcProcessor::HandlePoison() {
}

void TInterconnectZcProcessor::ExtractToSafeTermination(std::list<TEventHolder>& queue) {
    for (std::list<TEventHolder>::iterator event = queue.begin(); event != queue.end();) {
        if (event->ZcTransferId > LastZcConfirmed) {
            Delayed.splice(Delayed.end(), queue, event++);
        } else {
            event++;
        }
    }
}

ssize_t TInterconnectZcProcessor::ProcessSend(std::span<TConstIoVec> /*wbuf*/, TStreamSocket& /*socket*/,
    std::span<TOutgoingStream::TBufController> /*ctrl*/)
{
    Cerr << "ProcessSend" << Endl;
    return 0;
}

TInterconnectZcProcessor::TInterconnectZcProcessor(bool enabled)
    : TActor(&TInterconnectZcProcessor::StateFunc)
    , ZcState(enabled ? ZC_OK : ZC_DISABLED)
{}

}