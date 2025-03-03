#include "interconnect_zero_copy.h"

#include <util/datetime/cputimer.h>

#if defined (__linux__)
#define YDB_MSG_ZEROCOPY_SUPPORTED 1
#include <linux/errqueue.h>
#include <linux/netlink.h>
#endif 


#ifdef YDB_MSG_ZEROCOPY_SUPPORTED

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#endif


namespace NInterconnect {


#ifdef YDB_MSG_ZEROCOPY_SUPPORTED 
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

ssize_t TZeroCopyCtx::ProcessSend(TWriteRefVec& wbuffers, TStreamSocket& socket, NActors::IInterconnectMetrics* counters) {

#ifdef YDB_MSG_ZEROCOPY_SUPPORTED 
    if (ZcState == ZC_OK) {
        ssize_t zcPos = -1;
        for (size_t i = 0; i < wbuffers.size(); i++) {
            if (wbuffers[i].Size > ZcThreshold) {
                zcPos = i;
                break;
            }
        }

        if (wbuffers.size() > 1) {
            if (zcPos == 0) {
                wbuffers.resize(1);
            } else if (zcPos > 0) {
                wbuffers.resize((size_t)zcPos);
            }
        }
    }
#endif

    TString err;
    ssize_t r = 0;
    { // issue syscall with timing
        const ui64 begin = GetCycleCountFast();

        int flags = 0;
        do {
            if (wbuffers.size() == 1) {
                auto& front = wbuffers.front();
#ifdef YDB_MSG_ZEROCOPY_SUPPORTED 
                if (ZcState == ZC_OK && front.Size > ZcThreshold) {
                    flags = MSG_ZEROCOPY;
                }
                r = socket.SendWithFlags(front.Data, front.Size, flags);
#else
                r = socket.SendWithFlags(front.Data, front.Size, 0);
#endif
            } else {
                r = socket.WriteV(reinterpret_cast<const iovec*>(wbuffers.data()), wbuffers.size());
            }
        } while (r == -EINTR);

        if (flags == MSG_ZEROCOPY) {
            if (r > 0) {
                ZcUncompletedSend++;
            } else if (r == -ENOBUFS) {
                fprintf(stderr, "got enobuf: %lu\n", ZcUncompletedSend);
                // Got ENOBUFS just for first not completed zero copy transfer
                // it looks like misconfiguration (unable to lock page or net.core.optmem_max too small)
                // It is better just to stop trying using ZC
                if (ZcUncompletedSend == ZcSend) {
                    ZcState = ZC_DISABLED_ERR;
                } else {
                    ZcState = ZC_CONGESTED;
                }
                // The state changed. Trigger retry
                r = -EAGAIN;
            }
        }

        const ui64 end = GetCycleCountFast();

        counters->IncSendSyscalls((end - begin) * 1'000'000 / GetCyclesPerMillisecond());
    }
    return r;
}

#ifdef YDB_MSG_ZEROCOPY_SUPPORTED 
void TZeroCopyCtx::ProcessErrQueue(NInterconnect::TStreamSocket& socket) {
        // Mostly copy-paste from grpc ERRQUEUE handling
        struct iovec iov;
        iov.iov_base = nullptr;
        iov.iov_len = 0;
        struct msghdr msg;
        msg.msg_name = nullptr;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
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
            ZcState =ZC_OK;
        }
    }
#endif

TString TZeroCopyCtx::GetCurrentState() const {
    switch (ZcState) {
        case ZC_DISABLED:
            return "Disabled";
        case ZC_DISABLED_ERR:
            return "DisabledErr";
        case ZC_OK:
            return "Ok";
        case ZC_CONGESTED:
            return "Congested";
    }
}

TZeroCopyCtx::TZeroCopyCtx(bool enabled)
    : ZcState(enabled ? ZC_OK : ZC_DISABLED)
{}

std::unique_ptr<TZeroCopyCtx> MakeZeroCopyCtx(bool enabled) {
    return std::make_unique<TZeroCopyCtx>(enabled);
}

}