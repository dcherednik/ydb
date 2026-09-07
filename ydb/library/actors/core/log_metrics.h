#pragma once

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/monlib/metrics/metric_registry.h>
#include <library/cpp/monlib/service/pages/templates.h>

namespace NActors {
class ILoggerMetrics {
public:
    virtual ~ILoggerMetrics() = default;

    virtual void IncActorMsgs() = 0;
    virtual void IncDirectMsgs() = 0;
    virtual void IncLevelRequests() = 0;
    virtual void IncIgnoredMsgs() = 0;
    virtual void IncAlertMsgs() = 0;
    virtual void IncEmergMsgs() = 0;
    virtual void IncDroppedMsgs() = 0;

    virtual void GetOutputHtml(IOutputStream&) = 0;
};

class TLoggerCounters : public ILoggerMetrics {
public:
    TLoggerCounters(TIntrusivePtr<NMonitoring::TDynamicCounters> counters)
        : DynamicCounters(counters)
    {
        ActorMsgs_ = DynamicCounters->GetCounter("ActorMsgs", true);
        DirectMsgs_ = DynamicCounters->GetCounter("DirectMsgs", true);
        LevelRequests_ = DynamicCounters->GetCounter("LevelRequests", true);
        IgnoredMsgs_ = DynamicCounters->GetCounter("IgnoredMsgs", true);
        DroppedMsgs_ = DynamicCounters->GetCounter("DroppedMsgs", true);

        AlertMsgs_ = DynamicCounters->GetCounter("AlertMsgs", true);
        EmergMsgs_ = DynamicCounters->GetCounter("EmergMsgs", true);
    }

    ~TLoggerCounters() = default;

    void IncActorMsgs() override {
    }
    void IncDirectMsgs() override {
    }
    void IncLevelRequests() override {
    }
    void IncIgnoredMsgs() override {
    }
    void IncAlertMsgs() override {
    }
    void IncEmergMsgs() override {
    }
    void IncDroppedMsgs() override {
    }

    void GetOutputHtml(IOutputStream& str) override {
        HTML(str) {
            DIV_CLASS("row") {
                DIV_CLASS("col-md-12") {
                    TAG(TH4) {
                        str << "Counters" << Endl;
                    }
                    DynamicCounters->OutputHtml(str);
                }
            }
        }
    }

private:
    NMonitoring::TDynamicCounters::TCounterPtr ActorMsgs_;
    NMonitoring::TDynamicCounters::TCounterPtr DirectMsgs_;
    NMonitoring::TDynamicCounters::TCounterPtr LevelRequests_;
    NMonitoring::TDynamicCounters::TCounterPtr IgnoredMsgs_;
    NMonitoring::TDynamicCounters::TCounterPtr AlertMsgs_;
    NMonitoring::TDynamicCounters::TCounterPtr EmergMsgs_;
    // Dropped while the logger backend was unavailable
    NMonitoring::TDynamicCounters::TCounterPtr DroppedMsgs_;

    TIntrusivePtr<NMonitoring::TDynamicCounters> DynamicCounters;
};

class TLoggerMetrics : public ILoggerMetrics {
public:
    TLoggerMetrics(std::shared_ptr<NMonitoring::TMetricRegistry> metrics)
        : Metrics(metrics)
    {
        ActorMsgs_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.actor_msgs"}});
        DirectMsgs_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.direct_msgs"}});
        LevelRequests_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.level_requests"}});
        IgnoredMsgs_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.ignored_msgs"}});
        DroppedMsgs_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.dropped_msgs"}});

        AlertMsgs_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.alert_msgs"}});
        EmergMsgs_ = Metrics->Rate(NMonitoring::TLabels{{"sensor", "logger.emerg_msgs"}});
    }

    ~TLoggerMetrics() = default;

    void IncActorMsgs() override {
    }
    void IncDirectMsgs() override {
    }
    void IncLevelRequests() override {
    }
    void IncIgnoredMsgs() override {
    }
    void IncAlertMsgs() override {
    }
    void IncEmergMsgs() override {
    }
    void IncDroppedMsgs() override {
    }

    void GetOutputHtml(IOutputStream& str) override {
        HTML(str) {
            DIV_CLASS("row") {
                DIV_CLASS("col-md-12") {
                    TAG(TH4) {
                        str << "Metrics" << Endl;
                    }
                    // TODO: Now, TMetricRegistry does not have the GetOutputHtml function
                }
            }
        }
    }

private:
    NMonitoring::TRate* ActorMsgs_;
    NMonitoring::TRate* DirectMsgs_;
    NMonitoring::TRate* LevelRequests_;
    NMonitoring::TRate* IgnoredMsgs_;
    NMonitoring::TRate* AlertMsgs_;
    NMonitoring::TRate* EmergMsgs_;
    // Dropped while the logger backend was unavailable
    NMonitoring::TRate* DroppedMsgs_;

    std::shared_ptr<NMonitoring::TMetricRegistry> Metrics;
};
}
