#pragma once

#include <ydb/library/actors/core/actor.h>

namespace NInterconnect {

class TInterconnectZcProcessor  : public NActors::TActor<TInterconnectZcProcessor> {
    TInterconnectZcProcessor();

public:
    ~TInterconnectZcProcessor() = default;

    // Start termination process. All buffers will be freed only after compliting zero copy tasks
    void ScheduleTermination();

private:
    STATEFN(StateFunc);

    void HandlePoison();

public:
    static TInterconnectZcProcessor* Register(const NActors::TActorContext &ctx); 
};

}