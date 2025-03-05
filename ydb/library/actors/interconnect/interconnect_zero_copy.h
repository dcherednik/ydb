#pragma once

#include <memory>

#include <library/cpp/containers/stack_vector/stack_vec.h>
#include <ydb/library/actors/core/event_load.h>
#include <ydb/library/actors/interconnect/interconnect_counters.h>
#include <ydb/library/actors/interconnect/interconnect_stream.h>
#include <util/system/types.h>

namespace NInterconnect {

class TZeroCopyCtx {
public:
    TZeroCopyCtx(bool enabled);
    constexpr static ui32 IovLimit = 256;
    constexpr static ui32 ZcThreshold = 16384;
    using TWriteRefVec = TStackVec<NActors::TConstIoVec, IovLimit>;
    ssize_t ProcessSend(TWriteRefVec& bufs, TStreamSocket& socket, NActors::IInterconnectMetrics* counters, bool tryZc);
    void ResetState();
    void ProcessErrQueue(NInterconnect::TStreamSocket& socket);
    ui64 GetZcSend() const { return ZcSend; }
    ui64 GetZcSendWithCopy() const { return ZcSendWithCopy; }
    TString GetCurrentState() const;
    bool IsZcCompleted() const { return ZcUncompletedSend == ZcSend; }
    void Pause();

private:
    ui64 ZcUncompletedSend = 0;
    ui64 ZcSend = 0;
    ui64 ZcSendWithCopy = 0;

    TString LastErr;
    enum {
        ZC_DISABLED,            // ZeroCopy featute is disabled by used
        ZC_DISABLED_TMP,        // Temporary disabled due to transient state in the interconect (new session for example)
        ZC_DISABLED_ERR,        // We got some errors and unable to use ZC for this connection
        ZC_DISABLED_HIDEN_COPY, // The socket associated with loopback, or unsupported nic
                                // ZC send is not possible in this case and cause hiden copy inside kernel.
        ZC_OK,                  // OK
        ZC_CONGESTED,           // We got ENUBUF and temporary disable ZC send

    } ZcState;
};

std::unique_ptr<TZeroCopyCtx> MakeZeroCopyCtx(bool enabled);

}
