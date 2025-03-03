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
    ssize_t ProcessSend(TWriteRefVec& bufs, TStreamSocket& socket, NActors::IInterconnectMetrics* counters);
    void ProcessErrQueue(NInterconnect::TStreamSocket& socket);
    ui64 GetZcSend() const { return ZcSend; }
    ui64 GetZcSendWithCopy() const { return ZcSendWithCopy; }
    TString GetCurrentState() const;

private:
    ui64 ZcSend = 0;
    ui64 ZcSendWithCopy = 0;

    TString LastErr;
    enum {
        ZC_DISABLED,      // ZeroCopy featute is disabled by used
        ZC_DISABLED_ERR,  // We got some errors and unable to use ZC for this connection
        ZC_OK,            // OK
        ZC_CONGESTED,     // We got ENUBUF and temporary disable ZC send

    } ZcState;
};

std::unique_ptr<TZeroCopyCtx> MakeZeroCopyCtx(bool enabled);

}
