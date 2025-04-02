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

#ifdef YDB_MSG_ZEROCOPY_SUPPORTED

void TInterconnectZcProcessor::DoProcessErrQueue(NInterconnect::TStreamSocket& socket) {
    if (ZcState == ZC_DISABLED || ZcState == ZC_DISABLED_HIDEN_COPY) {
        return;
    }

    // Mostly copy-paste from grpc ERRQUEUE handling
    struct msghdr msg;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = nullptr;
    msg.msg_iovlen = 0;
    msg.msg_flags = 0;

    constexpr size_t cmsg_alloc_space =
        CMSG_SPACE(sizeof(scm_timestamping)) +
        CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in)) +
        CMSG_SPACE(32 * NLA_ALIGN(NLA_HDRLEN + sizeof(uint64_t)));

    union {
        char rbuf[cmsg_alloc_space];
        struct cmsghdr align;
    } aligned_buf;
    msg.msg_control = aligned_buf.rbuf;
    ssize_t r;
    while (true) {
        msg.msg_controllen = sizeof(aligned_buf.rbuf);

        do {
            r = socket.RecvErrQueue(&msg);
        } while (r == -EINTR);

        if (r == -EAGAIN || r == -EWOULDBLOCK) {
        //if (r < 0) {
            break;
        }

        if ((msg.msg_flags & MSG_CTRUNC) != 0) {
            ZcState = ZC_DISABLED_ERR;
            LastErr += "errqueue message was truncated;";
        }

        if (msg.msg_controllen == 0) {
            // There was no control message found. It was probably spurious.
            break;
        }
        for (auto cmsg = CMSG_FIRSTHDR(&msg); cmsg && cmsg->cmsg_len;
            cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (CmsgIsZeroCopy(*cmsg)) {
                auto serr = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(cmsg));
                fprintf(stderr, "err queue: %d ... %d\n", serr->ee_data, serr->ee_info);
                if (serr->ee_data < serr->ee_info) {
                    // Incorrect data inside kernel
                    continue;
                }
                ui64 sends = serr->ee_data - serr->ee_info + 1;
                ZcSend += sends;
                if (serr->ee_code == SO_EE_CODE_ZEROCOPY_COPIED) {
                    ZcSendWithCopy += sends;
                }
            }
        }
    }
    if (ZcState == ZC_CONGESTED && ZcSend == ZcUncompletedSend) {
        ZcState = ZC_OK;
    }

    // There are no reliable way to check is both side of tcp connection
    // place on the same host (consider different namespaces is the same host too).
    // So we check that each transfer has hidden copy during some period.
    if (ZcState == ZC_OK && ZcSendWithCopy == ZcSend && ZcSend > 10) {
        ZcState = ZC_DISABLED_HIDEN_COPY;
    }
}

#endif

// returns nunmber of buffers which should be sended to find ZC ready buffer on the firest place
size_t AdjustLen(std::span<const TConstIoVec> wbuf, std::span<const TOutgoingStream::TBufController> ctrl, ui64 threshold)
{
    size_t l = wbuf.size();
    for (size_t i = 0; i < wbuf.size(); i++) {
        if (ctrl[i].ZcReady() && wbuf[i].Size > threshold) {
            l = i;
            break;
        }
    }

    return l;
}

void TInterconnectZcProcessor::ProcessNotification(NInterconnect::TStreamSocket& socket) {
    DoProcessErrQueue(socket);
}

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

ssize_t TInterconnectZcProcessor::ProcessSend(std::span<TConstIoVec> wbuf, TStreamSocket& socket,
    std::span<TOutgoingStream::TBufController> ctrl)
{
    Y_DEBUG_ABORT_UNLESS(wbuf.size() == ctrl.size());
    size_t len = wbuf.size();

    if (ZcStateIsOk()) {
        len = AdjustLen(wbuf, ctrl, ZcThreshold);
    }

    ssize_t r = 0;
    int flags = 0;

    do {
        switch (len) {
            case 0:
#ifdef YDB_MSG_ZEROCOPY_SUPPORTED
                if (ZcStateIsOk()) {
                    flags |= MSG_ZEROCOPY;
                }
#endif
            case 1:
                r = socket.SendWithFlags(wbuf.front().Data, wbuf.front().Size, flags);
                break;
            default:
                r = socket.WriteV(reinterpret_cast<const iovec*>(wbuf.data()), len);
        }
    } while (r == -EINTR);

#ifdef YDB_MSG_ZEROCOPY_SUPPORTED
    if (flags & MSG_ZEROCOPY) {
        if (r > 0) {
            // Successful enqueued in to the kernel - increment counter to track dequeue progress
            ZcUncompletedSend++;
            ctrl.front().Update(ZcUncompletedSend);
        } else if (r == -ENOBUFS) {
                if (ZcUncompletedSend == ZcSend) {
                    // Got ENOBUFS just for first not completed zero copy transfer
                    // it looks like misconfiguration (unable to lock page or net.core.optmem_max extremely small)
                    // It is better just to stop trying using ZC
                    ZcState = ZC_DISABLED_ERR;
                } else {
                    // Got ENOBUFS after some successful send calls. Probably net.core.optmem_max still is not enought
                    // Just disable temporary ZC until we dequeue notifications
                    ZcState = ZC_CONGESTED;
                }
                // The state changed. Trigger retry
                r = -EAGAIN;
        }
    }
#endif

    Cerr << "ProcessSend: " << ctrl.size() << Endl;
    return r;
}

TInterconnectZcProcessor::TInterconnectZcProcessor(bool enabled)
    : TActor(&TInterconnectZcProcessor::StateFunc)
    , ZcState(enabled ? ZC_OK : ZC_DISABLED)
{}

TString TInterconnectZcProcessor::GetCurrentState() const {
    switch (ZcState) {
        case ZC_DISABLED:
            return "Disabled";
        case ZC_DISABLED_ERR:
            return "DisabledErr";
        case ZC_DISABLED_HIDEN_COPY:
            return "DisabledHidenCopy";
        case ZC_OK:
            return "Ok";
        case ZC_CONGESTED:
            return "Congested";
    }
}

}