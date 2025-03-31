#include "interconnect_zc_processor.h"

#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/core/hfunc.h>

namespace NInterconnect {
    using NActors::TEvents;

    TInterconnectZcProcessor*  TInterconnectZcProcessor::Register(const NActors::TActorContext &ctx) {
        TInterconnectZcProcessor* actor(new TInterconnectZcProcessor());
        ctx.RegisterWithSameMailbox(actor);
        return actor;
    }

    STFUNC(TInterconnectZcProcessor::StateFunc) {
        STRICT_STFUNC_BODY(
            cFunc(TEvents::TEvPoisonPill::EventType, HandlePoison)
        )
    }

    void TInterconnectZcProcessor::ScheduleTermination() {
        Cerr << "ScheduleTermination" << Endl;
    }

    void TInterconnectZcProcessor::HandlePoison() {
    }

    TInterconnectZcProcessor::TInterconnectZcProcessor()
        : TActor(&TInterconnectZcProcessor::StateFunc){}
}