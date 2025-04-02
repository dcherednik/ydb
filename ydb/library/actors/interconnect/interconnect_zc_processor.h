#pragma once

#include <ydb/library/actors/core/actor.h>

#include "interconnect_common.h"

#include "event_holder_pool.h"
#include "packet.h"

#include <list>

namespace NInterconnect {

class TInterconnectZcProcessor  : public NActors::TActor<TInterconnectZcProcessor> {
    TInterconnectZcProcessor(bool enabled);

public:
    ~TInterconnectZcProcessor() = default;

    void ExtractToSafeTermination(std::list<TEventHolder>& queue);

    // Start termination process. All buffers will be freed only after compliting zero copy tasks
    // Can be called only once
    void ScheduleTermination(std::unique_ptr<NActors::TEventHolderPool>&& pool);
    void SetSocket();
    ssize_t ProcessSend(std::span<TConstIoVec> wbuf, TStreamSocket& socket, std::span<TOutgoingStream::TBufController> ctrl);
    void ProcessNotification(NInterconnect::TStreamSocket& socket);

    constexpr static ui32 ZcThreshold = 16384;

private:
    ui64 ZcUncompletedSend = 0;
    ui64 ZcSend = 0;
    ui64 ZcSendWithCopy = 0;

    TString LastErr;

    STATEFN(StateFunc);

    void HandlePoison();

    ui32 LastZcConfirmed;

    std::list<TEventHolder> Delayed;
    std::unique_ptr<NActors::TEventHolderPool> Pool;

    void DoProcessErrQueue(NInterconnect::TStreamSocket& socket);


    enum {
        ZC_DISABLED,            // ZeroCopy featute is disabled by used
        ZC_DISABLED_TMP,        // Temporary disabled due to transient state in the interconect (reestablish connection)
        ZC_DISABLED_ERR,        // We got some errors and unable to use ZC for this connection
        ZC_DISABLED_HIDEN_COPY, // The socket associated with loopback, or unsupported nic
                                // real ZC send is not possible in this case and cause hiden copy inside kernel.
        ZC_OK,                  // OK, data can be send using zero copy
        ZC_CONGESTED,           // We got ENUBUF and temporary disable ZC send

    } ZcState;

    bool ZcStateIsOk() { return ZcState == ZC_OK; }

public:
    static TInterconnectZcProcessor* Register(const NActors::TActorContext &ctx, bool enabled); 
};

}