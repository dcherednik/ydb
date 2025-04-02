#pragma once

#include <ydb/library/actors/core/actor.h>

#include "interconnect_common.h"

#include "event_holder_pool.h"
#include "packet.h"

#include <list>

namespace NInterconnect {

class IZcGuard {
public:
    virtual ~IZcGuard() = default;
    virtual void ExtractToSafeTermination(std::list<TEventHolder>& queue) noexcept = 0;
    virtual void Terminate(std::unique_ptr<NActors::TEventHolderPool>&& pool, TIntrusivePtr<NInterconnect::TStreamSocket> socket, const NActors::TActorContext &ctx) = 0;
};

class TInterconnectZcProcessor { //}  : public NActors::TActor<TInterconnectZcProcessor> {
public:
    TInterconnectZcProcessor(bool enabled);
    ~TInterconnectZcProcessor() = default;

    ssize_t ProcessSend(std::span<TConstIoVec> wbuf, TStreamSocket& socket, std::span<TOutgoingStream::TBufController> ctrl);
    void ProcessNotification(NInterconnect::TStreamSocket& socket);

    std::unique_ptr<IZcGuard> GetGuard();

    ui64 GetZcSend() const { return ZcSend; }
    ui64 GetZcSendWithCopy() const { return ZcSendWithCopy; }
    TString GetCurrentState() const;

    constexpr static ui32 ZcThreshold = 16384;

private:
    ui64 ZcUncompletedSend = 0;
    ui64 ZcSend = 0;
    ui64 ZcSendWithCopy = 0;

    TString LastErr;



    ui32 LastZcConfirmed;



    void DoProcessErrQueue(NInterconnect::TStreamSocket& socket);

    enum {
        ZC_DISABLED,            // ZeroCopy featute is disabled by used
        ZC_DISABLED_ERR,        // We got some errors and unable to use ZC for this connection
        ZC_DISABLED_HIDEN_COPY, // The socket associated with loopback, or unsupported nic
                                // real ZC send is not possible in this case and cause hiden copy inside kernel.
        ZC_OK,                  // OK, data can be send using zero copy
        ZC_CONGESTED,           // We got ENUBUF and temporary disable ZC send

    } ZcState;

    bool ZcStateIsOk() { return ZcState == ZC_OK; }
};

}