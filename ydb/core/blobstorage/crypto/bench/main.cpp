#include <ydb/core/blobstorage/crypto/crypto.h>

#include <library/cpp/testing/benchmark/bench.h>

#include <util/generic/vector.h>
#include <util/system/align.h>

#include <cstring>

namespace {

constexpr size_t SectorSize = 4096;
constexpr size_t FooterSize = 24;
constexpr size_t CanarySize = 8;
constexpr size_t SectorPayloadSize = SectorSize - FooterSize - CanarySize;
constexpr size_t BufferAlignment = 64;
constexpr size_t StreamingSlots = (64 << 20) / SectorSize;

class TBenchmarkData {
    TVector<ui8> SourceStorage;
    TVector<ui8> DestinationStorage;

public:
    ui8* const Source;
    ui8* const Destination;

    TBenchmarkData()
        : SourceStorage(StreamingSlots * SectorSize + BufferAlignment)
        , DestinationStorage(StreamingSlots * SectorSize + BufferAlignment)
        , Source(reinterpret_cast<ui8*>(AlignUp(
              reinterpret_cast<uintptr_t>(SourceStorage.data()), BufferAlignment)))
        , Destination(reinterpret_cast<ui8*>(AlignUp(
              reinterpret_cast<uintptr_t>(DestinationStorage.data()), BufferAlignment)))
    {
        for (size_t i = 0; i < StreamingSlots * SectorSize; ++i) {
            Source[i] = static_cast<ui8>(i);
        }
    }
};

TBenchmarkData Data;

enum class EStrategy {
    // Reproduce the old alignment-dependent branch from TCompletionChunkReadPart::UnencryptData.
    LegacyInplaceAndCopy,

    // Decrypt directly to the destination whenever the input starts at the beginning of a sector payload.
    CurrentDirectDestination,

    // Advance the stream over the unused prefix, then decrypt the payload directly to its destination.
    AdvancePrefix,

    // Position the stream at the payload without touching the unused prefix.
    StartAtOffset,

    // Position independently at both the payload and the canary, without touching unused bytes.
    StartAtOffsetAndCanary,
};

template <EStrategy Strategy, size_t Offset, size_t PayloadSize, size_t DestinationShift, bool Streaming>
void RunDecryptBenchmark(NBench::NCpu::TParams& iface) {
    static_assert(Offset + PayloadSize <= SectorPayloadSize);
    static_assert(DestinationShift < BufferAlignment);

    NKikimr::TStreamCypher cypher;
    cypher.SetKey(123);

    for (size_t i = 0; i < iface.Iterations(); ++i) {
        const size_t slot = Streaming ? i & (StreamingSlots - 1) : 0;
        ui8* const source = Data.Source + slot * SectorSize;
        ui8* const destination = Data.Destination + slot * SectorSize + DestinationShift;
        const ui64 nonce = 1 + slot;

        NBench::Escape(source);
        NBench::Escape(destination);

        if constexpr (Strategy == EStrategy::LegacyInplaceAndCopy) {
            cypher.StartMessage(nonce, 0);
            if constexpr (Offset != 0 || DestinationShift % 32 != 0) {
                cypher.InplaceEncrypt(source, Offset + PayloadSize);
                std::memcpy(destination, source + Offset, PayloadSize);
            } else {
                cypher.Encrypt(destination, source, PayloadSize);
            }
        } else if constexpr (Strategy == EStrategy::CurrentDirectDestination) {
            cypher.StartMessage(nonce, 0);
            if constexpr (Offset != 0) {
                cypher.InplaceEncrypt(source, Offset + PayloadSize);
                std::memcpy(destination, source + Offset, PayloadSize);
            } else {
                cypher.Encrypt(destination, source, PayloadSize);
            }
        } else if constexpr (Strategy == EStrategy::AdvancePrefix) {
            cypher.StartMessage(nonce, 0);
            if constexpr (Offset != 0) {
                cypher.InplaceEncrypt(source, Offset);
            }
            cypher.Encrypt(destination, source + Offset, PayloadSize);
        } else {
            cypher.StartMessage(nonce, Offset);
            cypher.Encrypt(destination, source + Offset, PayloadSize);
        }

        if constexpr (Strategy == EStrategy::StartAtOffsetAndCanary) {
            cypher.StartMessage(nonce, SectorPayloadSize);
            cypher.InplaceEncrypt(source + SectorPayloadSize, CanarySize);
        } else {
            constexpr size_t canaryPosition = Offset + PayloadSize;
            constexpr size_t bytesToCanaryEnd = SectorSize - canaryPosition - FooterSize;
            cypher.InplaceEncrypt(source + canaryPosition, bytesToCanaryEnd);
        }

        NBench::Clobber();
    }
}

#define DEFINE_BENCHMARK(NAME, STRATEGY, OFFSET, PAYLOAD_SIZE, DESTINATION_SHIFT, STREAMING) \
    Y_CPU_BENCHMARK(NAME, iface) {                                                        \
        RunDecryptBenchmark<EStrategy::STRATEGY, OFFSET, PAYLOAD_SIZE,                    \
            DESTINATION_SHIFT, STREAMING>(iface);                                         \
    }

// Full sector payload. The unaligned destination triggers the expensive branch in the current implementation.
DEFINE_BENCHMARK(Hot_Full_Aligned_Legacy, LegacyInplaceAndCopy, 0, SectorPayloadSize, 0, false)
DEFINE_BENCHMARK(Hot_Full_Aligned_Current, CurrentDirectDestination, 0, SectorPayloadSize, 0, false)
DEFINE_BENCHMARK(Hot_Full_Aligned_AdvancePrefix, AdvancePrefix, 0, SectorPayloadSize, 0, false)
DEFINE_BENCHMARK(Hot_Full_Aligned_StartAtOffset, StartAtOffset, 0, SectorPayloadSize, 0, false)
DEFINE_BENCHMARK(Hot_Full_Aligned_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 0, SectorPayloadSize, 0, false)

DEFINE_BENCHMARK(Hot_Full_Dst1_Legacy, LegacyInplaceAndCopy, 0, SectorPayloadSize, 1, false)
DEFINE_BENCHMARK(Hot_Full_Dst1_Current, CurrentDirectDestination, 0, SectorPayloadSize, 1, false)
DEFINE_BENCHMARK(Hot_Full_Dst1_AdvancePrefix, AdvancePrefix, 0, SectorPayloadSize, 1, false)
DEFINE_BENCHMARK(Hot_Full_Dst1_StartAtOffset, StartAtOffset, 0, SectorPayloadSize, 1, false)
DEFINE_BENCHMARK(Hot_Full_Dst1_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 0, SectorPayloadSize, 1, false)

// First partial sector of an unaligned read.
DEFINE_BENCHMARK(Hot_HeadOffset1_Legacy, LegacyInplaceAndCopy, 1, SectorPayloadSize - 1, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset1_Current, CurrentDirectDestination, 1, SectorPayloadSize - 1, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset1_AdvancePrefix, AdvancePrefix, 1, SectorPayloadSize - 1, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset1_StartAtOffset, StartAtOffset, 1, SectorPayloadSize - 1, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset1_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 1, SectorPayloadSize - 1, 0, false)

DEFINE_BENCHMARK(Hot_HeadOffset31_Legacy, LegacyInplaceAndCopy, 31, SectorPayloadSize - 31, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset31_Current, CurrentDirectDestination, 31, SectorPayloadSize - 31, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset31_AdvancePrefix, AdvancePrefix, 31, SectorPayloadSize - 31, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset31_StartAtOffset, StartAtOffset, 31, SectorPayloadSize - 31, 0, false)
DEFINE_BENCHMARK(Hot_HeadOffset31_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 31, SectorPayloadSize - 31, 0, false)

// Small partial reads expose the cost of decrypting bytes between the requested payload and the canary.
DEFINE_BENCHMARK(Hot_512B_Legacy, LegacyInplaceAndCopy, 17, 512, 0, false)
DEFINE_BENCHMARK(Hot_512B_Current, CurrentDirectDestination, 17, 512, 0, false)
DEFINE_BENCHMARK(Hot_512B_AdvancePrefix, AdvancePrefix, 17, 512, 0, false)
DEFINE_BENCHMARK(Hot_512B_StartAtOffset, StartAtOffset, 17, 512, 0, false)
DEFINE_BENCHMARK(Hot_512B_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 17, 512, 0, false)

DEFINE_BENCHMARK(Hot_64B_Legacy, LegacyInplaceAndCopy, 31, 64, 0, false)
DEFINE_BENCHMARK(Hot_64B_Current, CurrentDirectDestination, 31, 64, 0, false)
DEFINE_BENCHMARK(Hot_64B_AdvancePrefix, AdvancePrefix, 31, 64, 0, false)
DEFINE_BENCHMARK(Hot_64B_StartAtOffset, StartAtOffset, 31, 64, 0, false)
DEFINE_BENCHMARK(Hot_64B_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 31, 64, 0, false)

// Cycle through a 64 MiB source and destination to measure memory traffic rather than just hot-cache execution.
DEFINE_BENCHMARK(Streaming_Full_Aligned_Legacy, LegacyInplaceAndCopy, 0, SectorPayloadSize, 0, true)
DEFINE_BENCHMARK(Streaming_Full_Aligned_Current, CurrentDirectDestination, 0, SectorPayloadSize, 0, true)
DEFINE_BENCHMARK(Streaming_Full_Aligned_AdvancePrefix, AdvancePrefix, 0, SectorPayloadSize, 0, true)
DEFINE_BENCHMARK(Streaming_Full_Aligned_StartAtOffset, StartAtOffset, 0, SectorPayloadSize, 0, true)
DEFINE_BENCHMARK(Streaming_Full_Aligned_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 0, SectorPayloadSize, 0, true)

DEFINE_BENCHMARK(Streaming_Full_Dst1_Legacy, LegacyInplaceAndCopy, 0, SectorPayloadSize, 1, true)
DEFINE_BENCHMARK(Streaming_Full_Dst1_Current, CurrentDirectDestination, 0, SectorPayloadSize, 1, true)
DEFINE_BENCHMARK(Streaming_Full_Dst1_AdvancePrefix, AdvancePrefix, 0, SectorPayloadSize, 1, true)
DEFINE_BENCHMARK(Streaming_Full_Dst1_StartAtOffset, StartAtOffset, 0, SectorPayloadSize, 1, true)
DEFINE_BENCHMARK(Streaming_Full_Dst1_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 0, SectorPayloadSize, 1, true)

DEFINE_BENCHMARK(Streaming_HeadOffset1_Legacy, LegacyInplaceAndCopy, 1, SectorPayloadSize - 1, 0, true)
DEFINE_BENCHMARK(Streaming_HeadOffset1_Current, CurrentDirectDestination, 1, SectorPayloadSize - 1, 0, true)
DEFINE_BENCHMARK(Streaming_HeadOffset1_AdvancePrefix, AdvancePrefix, 1, SectorPayloadSize - 1, 0, true)
DEFINE_BENCHMARK(Streaming_HeadOffset1_StartAtOffset, StartAtOffset, 1, SectorPayloadSize - 1, 0, true)
DEFINE_BENCHMARK(Streaming_HeadOffset1_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 1, SectorPayloadSize - 1, 0, true)

DEFINE_BENCHMARK(Streaming_512B_Legacy, LegacyInplaceAndCopy, 17, 512, 0, true)
DEFINE_BENCHMARK(Streaming_512B_Current, CurrentDirectDestination, 17, 512, 0, true)
DEFINE_BENCHMARK(Streaming_512B_AdvancePrefix, AdvancePrefix, 17, 512, 0, true)
DEFINE_BENCHMARK(Streaming_512B_StartAtOffset, StartAtOffset, 17, 512, 0, true)
DEFINE_BENCHMARK(Streaming_512B_StartAtOffsetAndCanary, StartAtOffsetAndCanary, 17, 512, 0, true)

#undef DEFINE_BENCHMARK

} // anonymous namespace
