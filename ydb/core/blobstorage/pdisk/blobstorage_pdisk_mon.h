#pragma once

#include <ydb/core/blobstorage/base/common_latency_hist_bounds.h>
#include <ydb/core/blobstorage/lwtrace_probes/blobstorage_probes.h>
#include <ydb/core/base/blobstorage_write_source.h>
#include <ydb/core/mon/mon.h>
#include <ydb/core/mon/noop_counter.h>
#include <ydb/core/protos/blobstorage_disk.pb.h>
#include <ydb/core/protos/node_whiteboard.pb.h>
#include <ydb/core/util/light.h>
#include <ydb/core/util/max_tracker.h>

#include <library/cpp/bucket_quoter/bucket_quoter.h>
#include <library/cpp/containers/stack_vector/stack_vec.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/monlib/dynamic_counters/percentile/percentile_lg.h>
#include <util/generic/vector.h>


namespace NKikimr {

struct TPDiskConfig;

class TNoopPercentileTracker {
public:
    template <typename... TArgs>
    void Initialize(TArgs&&...) {}

    template <typename T>
    void Increment(T) {}

    void Update() {}
};

class TBurstmeter {
private:
    TBucketQuoter<i64, TSpinLock, THPTimerUs> Bucket;
    TNoopPercentileTracker Tracker;
public:
    TBurstmeter()
        : Bucket(1000ull * 1000ull * 1000ull, 0)
    {}

    void Initialize(const TIntrusivePtr<::NMonitoring::TDynamicCounters> &counters,
                    const TString& group, const TString& subgroup, const TString& name,
                    const TVector<float> &thresholds,
                    NMonitoring::TCountableBase::EVisibility visibility = NMonitoring::TCountableBase::EVisibility::Public) {
        Tracker.Initialize(counters, group, subgroup, name, thresholds, visibility);
    }

    double Increment(ui64 tokens) {
        double burst = -double(Bucket.UseAndFill(tokens)) / (1000000ull);
        return burst;
    }

    void Update() {
    }
};

class TBaseHistogram {
protected:
    NMonitoring::THistogramPtr Histo;

public:
    virtual ~TBaseHistogram() = default;

    virtual void Initialize(const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters,
                           const TString &name, NPDisk::EDeviceType deviceType) = 0;

    void Increment(double value) {
        Y_UNUSED(value);
    }
};

class TTimesHistogram : public TBaseHistogram {
public:
    void Initialize(const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters,
            const TString &name, NPDisk::EDeviceType deviceType) override {
        TString histName = name + "Ms";
        // Histogram buckets in milliseconds
        auto h = NMonitoring::ExplicitHistogram(GetCommonLatencyHistBounds(deviceType));
        Histo = counters->GetNamedHistogram("sensor", histName, std::move(h));
    }
};

class TBytesHistogram : public TBaseHistogram {
public:
    void Initialize(const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters,
            const TString &name, NPDisk::EDeviceType deviceType) override {
        Y_UNUSED(deviceType);
        TString histName = name + "KB";
        // Histogram buckets in KB
        TVector<double> bounds = {1_KB, 2_KB,4_KB, 8_KB, 16_KB, 32_KB, 64_KB};
        auto h = NMonitoring::ExplicitHistogram(bounds);
        Histo = counters->GetNamedHistogram("sensor", histName, std::move(h));
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PDisk monitoring counters
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct TPDiskMon {
    struct TPDisk {
        enum EBriefState {
            Booting,
            OK,
            Error,
            Stopped
        };

        enum EDetailedState {
            EverythingIsOk,
            BootingFormatRead,
            BootingSysLogRead,
            BootingCommonLogRead,
            BootingFormatMagicChecking,
            BootingDeviceFormattingAndTrimming,
            ErrorInitialFormatRead, // deprecated; kept for backward compatibility, replaced with two following states
            ErrorInitialFormatReadDueToGuid,
            ErrorInitialFormatReadIncompleteFormat,
            ErrorDiskCannotBeFormated,
            ErrorPDiskCannotBeInitialised,
            ErrorInitialSysLogRead,
            ErrorInitialSysLogParse,
            ErrorInitialCommonLogRead,
            ErrorInitialCommonLogParse,
            ErrorCommonLoggerInit,
            ErrorOpenNonexistentFile,
            ErrorOpenFileWithoutPermissions,
            ErrorOpenFileUnknown,
            ErrorCalculatingChunkQuotas,
            ErrorDeviceIoError,
            ErrorNoDeviceWithSuchSerial,
            ErrorDeviceSerialMismatch,
            ErrorFake,
            BootingReencryptingFormat,
            StoppedByYardControl,
        };

        static TString StateToStr(i64 val) {
            return NKikimrBlobStorage::TPDiskState::E_Name(static_cast<NKikimrBlobStorage::TPDiskState::E>(val));
        }

        static const char *BriefStateToStr(i64 val) {
            switch (val) {
                case Booting: return "Booting";
                case OK: return "OK";
                case Error: return "Error";
                case Stopped: return "Stopped";
                default: return "Unknown";
            }
        }

        static const char *DetailedStateToStr(i64 val) {
            switch (val) {
                case EverythingIsOk: return "EverythingIsOk";
                case BootingFormatRead: return "BootingFormatRead";
                case BootingSysLogRead: return "BootingSysLogRead";
                case BootingCommonLogRead: return "BootingCommonLogRead";
                case BootingFormatMagicChecking: return "BootingFormatMagicChecking";
                case BootingDeviceFormattingAndTrimming: return "BootingDeviceFormattingAndTrimming";
                case ErrorInitialFormatRead: return "ErrorInitialFormatRead";
                case ErrorInitialFormatReadDueToGuid: return "ErrorInitialFormatReadDueToGuid";
                case ErrorInitialFormatReadIncompleteFormat: return "ErrorInitialFormatReadIncompleteFormat";
                case ErrorDiskCannotBeFormated: return "ErrorDiskCannotBeFormated";
                case ErrorPDiskCannotBeInitialised: return "ErrorPDiskCannotBeInitialised";
                case ErrorInitialSysLogRead: return "ErrorInitialSysLogRead";
                case ErrorInitialSysLogParse: return "ErrorInitialSysLogParse";
                case ErrorInitialCommonLogRead: return "ErrorInitialCommonLogRead";
                case ErrorInitialCommonLogParse: return "ErrorInitialCommonLogParse";
                case ErrorCommonLoggerInit: return "ErrorCommonLoggerInit";
                case ErrorOpenNonexistentFile: return "ErrorOpenNonexistentFile";
                case ErrorOpenFileWithoutPermissions: return "ErrorOpenFileWithoutPermissions";
                case ErrorOpenFileUnknown: return "ErrorOpenFileUnknown";
                case ErrorCalculatingChunkQuotas: return "ErrorCalculatingChunkQuotas";
                case ErrorDeviceIoError: return "ErrorDeviceIoError";
                case ErrorNoDeviceWithSuchSerial: return "ErrorNoDeviceWithSuchSerial";
                case ErrorDeviceSerialMismatch: return "ErrorDeviceSerialMismatch";
                case ErrorFake: return "ErrorFake";
                case BootingReencryptingFormat: return "BootingReencryptingFormat";
                case StoppedByYardControl: return "StoppedByYardControl";
                default: return "Unknown";
            }
        }
    };

    class TUpdateDurationTracker {
        bool IsLwProbeEnabled = false;
        NHPTimer::STime BeginUpdateAt = 0;
        NHPTimer::STime SchedulingStartAt = 0;
        NHPTimer::STime ProcessingStartAt = 0;
        NHPTimer::STime WaitingStartAt = 0;

        TNoopCounter PDiskThreadBusyTimeNs;

        ui32 PDiskId = 0;

    public:
        TNoopPercentileTracker UpdateCycleTime;

    public:
        TUpdateDurationTracker()
            : BeginUpdateAt(HPNow())
        {}

        void SetPDiskId(ui32 pdiskId) {
            PDiskId = pdiskId;
        }

        void SetCounter(const ::NMonitoring::TDynamicCounters::TCounterPtr& pDiskThreadBusyTimeNs) {
            PDiskThreadBusyTimeNs = pDiskThreadBusyTimeNs;
        }

        void UpdateStarted() {
            // BeginUpdateAt is set on the end of previous update cycle
            IsLwProbeEnabled = GLOBAL_LWPROBE_ENABLED(BLOBSTORAGE_PROVIDER, PDiskUpdateCycleDetails);
        }

        void SchedulingStart() {
            if (IsLwProbeEnabled) {
                SchedulingStartAt = HPNow();
            }
        }

        void ProcessingStart() {
            if (IsLwProbeEnabled) {
                ProcessingStartAt = HPNow();
            }
        }

        void WaitingStart(bool isNothingToDo) {
            const auto now = HPNow();
            if (PDiskThreadBusyTimeNs) {
                *PDiskThreadBusyTimeNs += HPNanoSeconds(now - BeginUpdateAt);
            }
            if (IsLwProbeEnabled || !isNothingToDo) {
                WaitingStartAt = now;
                if (!isNothingToDo) {
                    ui64 durationMs = HPMilliSeconds(WaitingStartAt - BeginUpdateAt);
                    UpdateCycleTime.Increment(durationMs);
                }
            }
        }

        float UpdateEnded() {
            NHPTimer::STime updateEndedAt = HPNow();
            float entireUpdateMs = HPMilliSecondsFloat(updateEndedAt - BeginUpdateAt);
            if (IsLwProbeEnabled) {
                float inputQueueMs = HPMilliSecondsFloat(SchedulingStartAt - BeginUpdateAt);
                float schedulingMs = HPMilliSecondsFloat(ProcessingStartAt - SchedulingStartAt);
                float processingMs = HPMilliSecondsFloat(WaitingStartAt - ProcessingStartAt);
                float waitingMs = HPMilliSecondsFloat(updateEndedAt - WaitingStartAt);
                GLOBAL_LWPROBE(BLOBSTORAGE_PROVIDER, PDiskUpdateCycleDetails, PDiskId, entireUpdateMs, inputQueueMs,
                        schedulingMs, processingMs, waitingMs);
            }
            BeginUpdateAt = updateEndedAt;
            return entireUpdateMs;
        }
    };

    TIntrusivePtr<::NMonitoring::TDynamicCounters> Counters;
    ui32 PDiskId;

    // chunk states subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> ChunksGroup;
    TNoopCounter UntrimmedFreeChunks;
    TNoopCounter FreeChunks;
    TNoopCounter LogChunks;
    TNoopCounter UncommitedDataChunks;
    TNoopCounter CommitedDataChunks;
    TNoopCounter LockedChunks;
    TNoopCounter QuarantineChunks;
    TNoopCounter QuarantineOwners;

    // statistics subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> StatsGroup;
    TNoopCounter FreeSpacePerMile;
    TNoopCounter UsedSpacePerMile; // reflects PDiskUsage
    TNoopCounter SplicedLogChunks;

    TNoopCounter TotalSpaceBytes;
    TNoopCounter FreeSpaceBytes;
    TNoopCounter UsedSpaceBytes;
    TNoopCounter SectorMapAllocatedBytes;

    TNoopCounter NumActiveSlots;
    TNoopCounter ExpectedSlotCount;
    TNoopCounter SlotSizeBytes;

    // states subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> StateGroup;
    TNoopCounter PDiskState;
    TNoopCounter PDiskBriefState;
    TNoopCounter PDiskDetailedState;
    TNoopCounter AtLeastOneVDiskNotLogged;
    TNoopCounter TooMuchLogChunks;
    TNoopCounter SerialNumberMismatched;
    TLight L6;
    TLight L7;
    TLight IdleLight;
    TNoopCounter OwnerIdsIssued;
    TNoopCounter LastOwnerId;
    TNoopCounter PendingYardInits;

    TAtomic SeqnoL6;
    TAtomic LastDoneOperationTimestamp;

    // device subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> DeviceGroup;
    TNoopCounter DeviceBytesRead;
    TNoopCounter DeviceBytesWritten;
    TNoopCounter DeviceReads;
    TNoopCounter DeviceWrites;
    TNoopCounter DeviceInFlightBytesRead;
    TNoopCounter DeviceInFlightBytesWrite;
    TNoopCounter DeviceInFlightReads;
    TMaxTracker MaxDeviceInFlightReads;
    TNoopCounter DeviceInFlightWrites;
    TMaxTracker MaxDeviceInFlightWrites;
    TNoopCounter DeviceTakeoffs;
    TNoopCounter DeviceLandings;
    TNoopCounter DeviceHaltDetected;
    TNoopCounter DeviceExpectedSeeks;
    TNoopCounter DeviceReadCacheHits;
    TNoopCounter DeviceReadCacheMisses;
    TNoopCounter DeviceWriteCacheIsValid;
    TNoopCounter DeviceWriteCacheIsEnabled;
    TNoopCounter DeviceOperationPoolTotalAllocations;
    TNoopCounter DeviceOperationPoolFreeObjectsMin;
    TNoopCounter DeviceBufferPoolFailedAllocations;
    TNoopCounter DeviceErasureSectorRestorations;
    TNoopCounter DeviceEstimatedCostNs;
    TNoopCounter DeviceActualCostNs;
    TNoopCounter DeviceOverestimationRatio;
    TNoopCounter DeviceNonperformanceMs;
    TNoopCounter DeviceInterruptedSystemCalls;
    TNoopCounter DeviceSubmitThreadBusyTimeNs;
    TNoopCounter DeviceCompletionThreadBusyTimeNs;
    TNoopCounter DeviceIoErrors;
    TNoopCounter DeviceWaitTimeMs;

    TBytesHistogram DeviceWritesSizes;

    // queue subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> QueueGroup;
    TNoopCounter QueueRequests;
    TNoopCounter QueueBytes;

    // Update cycle time
    TUpdateDurationTracker UpdateDurationTracker;

    // Device times
    TTimesHistogram DeviceReadDuration;
    TTimesHistogram DeviceWriteDuration;
    TTimesHistogram DeviceTrimDuration;
    TTimesHistogram DeviceFlushDuration;

    // <BASE_BITS, EXP_BITS, FRAME_COUNT>
    using TDurationTracker = TNoopPercentileTracker;
    // log queue duration
    TDurationTracker LogQueueTime;
    // get queue duration
    TDurationTracker GetQueueSyncLog;
    TDurationTracker GetQueueHullComp;
    TDurationTracker GetQueueHullOnlineRt;
    TDurationTracker GetQueueHullOnlineOther;
    TDurationTracker GetQueueHullLoad;
    TDurationTracker GetQueueHullLow;
    // write queue duration
    TDurationTracker WriteQueueSyncLog;
    TDurationTracker WriteQueueHullFresh;
    TDurationTracker WriteQueueHullHugeAsync;
    TDurationTracker WriteQueueHullHugeUser;
    TDurationTracker WriteQueueHullComp;

    // incoming flow burstiness
    TBurstmeter SensitiveBurst;
    TBurstmeter BestEffortBurst;

    // queue length seen by arriving request in front of it (QLA = Queue Length at Arrival)
    using TQLATracker = TNoopPercentileTracker;
    TQLATracker InputQLA; // for PDisk.InputQueue

    // queue cost seen by arriving request in front of it (QCA = Queue Cost at Arrival)
    using TQCATracker = TNoopPercentileTracker;
    TQCATracker InputQCA; // for PDisk.InputQueue

    // log cumulative size bytes
    // <BASE_BITS, EXP_BITS, FRAME_COUNT>
    using TSizeTracker = TNoopPercentileTracker;
    TSizeTracker LogOperationSizeBytes;
    TSizeTracker GetSyncLogSizeBytes;

    TSizeTracker GetHullCompSizeBytes;
    TSizeTracker GetHullOnlineRtSizeBytes;
    TSizeTracker GetHullOnlineOtherSizeBytes;
    TSizeTracker GetHullLoadSizeBytes;
    TSizeTracker GetHullLowSizeBytes;

    TSizeTracker WriteSyncLogSizeBytes;
    TSizeTracker WriteHullFreshSizeBytes;
    TSizeTracker WriteHullHugeAsyncSizeBytes;
    TSizeTracker WriteHullHugeUserSizeBytes;
    TSizeTracker WriteHullCompSizeBytes;

    // log response time
    TTimesHistogram LogResponseTime;
    // get response time
    TTimesHistogram GetResponseSyncLog;
    TTimesHistogram GetResponseHullComp;
    TTimesHistogram GetResponseHullOnlineRt;
    TTimesHistogram GetResponseHullOnlineOther;
    TTimesHistogram GetResponseHullLoad;
    TTimesHistogram GetResponseHullLow;
    // write response time
    TTimesHistogram WriteResponseSyncLog;
    TTimesHistogram WriteResponseHullFresh;
    TTimesHistogram WriteResponseHullHugeAsync;
    TTimesHistogram WriteResponseHullHugeUser;
    TTimesHistogram WriteResponseHullComp;

    // scheduler subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> SchedulerGroup;
    TNoopCounter ForsetiCbsNotFound;

    // bandwidth subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> BandwidthGroup;
    TNoopCounter BandwidthPLogPayload;
    TNoopCounter BandwidthPLogCommit;
    TNoopCounter BandwidthPLogSectorFooter;
    TNoopCounter BandwidthPLogRecordHeader;
    TNoopCounter BandwidthPLogPadding;
    TNoopCounter BandwidthPLogErasure;
    TNoopCounter BandwidthPLogChunkPadding;
    TNoopCounter BandwidthPLogChunkFooter;

    TNoopCounter BandwidthPSysLogPayload;
    TNoopCounter BandwidthPSysLogSectorFooter;
    TNoopCounter BandwidthPSysLogRecordHeader;
    TNoopCounter BandwidthPSysLogPadding;
    TNoopCounter BandwidthPSysLogErasure;

    TNoopCounter BandwidthPChunkPayload;
    TNoopCounter BandwidthPChunkSectorFooter;
    TNoopCounter BandwidthPChunkPadding;

    TNoopCounter BandwidthPChunkReadPayload;
    TNoopCounter BandwidthPChunkReadSectorFooter;

    TNoopCounter WriteBufferCompactedBytes;

    struct TIoCounters {
        TNoopCounter Requests;
        TNoopCounter Bytes;
        TNoopCounter Results;

        void Setup(const TIntrusivePtr<::NMonitoring::TDynamicCounters>& group, TString name, NMonitoring::TCountableBase::EVisibility vis) {
            TIntrusivePtr<::NMonitoring::TDynamicCounters> subgroup = group->GetSubgroup("req", name);
            Requests = subgroup->GetCounter("Requests", true, vis);
            Bytes = subgroup->GetCounter("Bytes", true, vis);
            Results = subgroup->GetCounter("Results", true, vis);
        }

        void CountRequest(ui32 size) {
            Requests->Inc();
            *Bytes += size;
        }

        void CountRequest() {
            Requests->Inc();
        }

        void CountResponse() {
            Results->Inc();
        }

        void CountResponse(ui32 size) {
            Results->Inc();
            *Bytes += size;
        }

        void CountMultipleResponses(ui32 num) {
            Results->Add(num);
        }
    };

    struct TReqCounters {
        TNoopCounter Requests;
        TNoopCounter Results;

        void Setup(const TIntrusivePtr<::NMonitoring::TDynamicCounters>& group, TString name, NMonitoring::TCountableBase::EVisibility vis) {
            TIntrusivePtr<::NMonitoring::TDynamicCounters> subgroup = group->GetSubgroup("req", name);
            Requests = subgroup->GetCounter("Requests", true, vis);
            Results = subgroup->GetCounter("Results", true, vis);
        }

        void CountRequest() {
            Requests->Inc();
        }

        void CountResponse() {
            Results->Inc();
        }
    };

    struct TOpCounters {
        TNoopCounter Requests;
        TNoopCounter Bytes;

        void Setup(TString metricPrefix, const TIntrusivePtr<::NMonitoring::TDynamicCounters>& group, TString opName,
                NMonitoring::TCountableBase::EVisibility vis) {
            TIntrusivePtr<::NMonitoring::TDynamicCounters> subgroup = group->GetSubgroup("op", opName);
            Requests = subgroup->GetCounter(metricPrefix + "RequestsByOp", true, vis);
            Bytes = subgroup->GetCounter(metricPrefix + "BytesByOp", true, vis);
        }

        void CountRequest(ui32 size) {
            Requests->Inc();
            *Bytes += size;
        }
    };

    // yard subgroup
    TIntrusivePtr<::NMonitoring::TDynamicCounters> PDiskGroup;
    TReqCounters YardInit;
    TReqCounters ChangeExpectedSlotCount;
    TReqCounters CheckSpace;
    TReqCounters YardConfigureScheduler;
    TReqCounters ChunkReserve;
    TReqCounters ChunkForget;
    TReqCounters Harakiri;
    TReqCounters YardSlay;
    TReqCounters YardControl;

    TReqCounters ShredPDisk;
    TReqCounters PreShredCompactVDisk;
    TReqCounters ShredVDiskResult;
    TReqCounters MarkDirty;

    TIoCounters WriteSyncLog;
    TIoCounters WriteFresh;
    TIoCounters WriteHugeAsync;
    TIoCounters WriteHugeUser;
    TIoCounters WriteComp;
    TIoCounters Trim;
    TIoCounters ChunkShred;
    TIoCounters ReadSyncLog;
    TIoCounters ReadComp;
    TIoCounters ReadOnlineRt;
    TIoCounters ReadOnlineOther;
    TIoCounters ReadLoad;
    TIoCounters ReadLow;

    TIoCounters Unknown;

    TIoCounters WriteLog;
    TReqCounters WriteHugeLog;
    TIoCounters LogRead;
    TVector<TOpCounters> LogWriteOpCounters;
    TVector<TOpCounters> ChunkWriteOpCounters;

public:
    // Halter
    i64 LastHaltDeviceTakeoffs = 0;
    i64 LastHaltDeviceLandings = 0;
    NHPTimer::STime LastHaltTimestamp = 0;

    // System counters - for tracking usage of CPU, memory etc.
    TIntrusivePtr<::NMonitoring::TDynamicCounters> SystemGroup;
    TNoopCounter PDiskThreadCPU;
    TNoopCounter SubmitThreadCPU;
    TNoopCounter GetThreadCPU;
    TNoopCounter TrimThreadCPU;
    TNoopCounter CompletionThreadCPU;

    TPDiskMon(const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters, ui32 pdiskId, TPDiskConfig *cfg);

    ::NMonitoring::TDynamicCounters::TCounterPtr GetBusyPeriod(const TString& owner, const TString& queue);
    void IncrementQueueTime(ui8 priorityClass, size_t timeMs);
    void IncrementResponseTime(ui8 priorityClass, double timeMs, size_t sizeBytes);
    void UpdatePercentileTrackers();
    void UpdateLights();
    bool UpdateDeviceHaltCounters();
    void UpdateStats();
    void CountLogWriteOpRequest(const TWriteSource& source, ui32 size);
    void CountChunkWriteOpRequest(const TWriteSource& source, ui32 size);
    TIoCounters *GetWriteCounter(ui8 priority);
    TIoCounters *GetReadCounter(ui8 priority);
};

} // NKikimr
