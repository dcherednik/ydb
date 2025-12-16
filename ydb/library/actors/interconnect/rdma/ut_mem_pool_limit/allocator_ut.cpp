#include <ydb/library/actors/interconnect/rdma/mem_pool.h>
#include <ydb/library/actors/interconnect/rdma/ut/utils/utils.h>

#include <library/cpp/testing/gtest/gtest.h>
#include <ydb/library/testlib/unittest_gtest_macro_subst.h>

#include <util/random/fast.h>
#include <util/random/random.h>

#include <thread>

namespace NMonitoring {
    struct TDynamicCounters;
}

static void GTestSkip() {
    GTEST_SKIP() << "Skipping all rdma tests for suit, set \""
                 << NRdmaTest::RdmaTestEnvSwitchName << "\" env if it is RDMA compatible";
}

class TAllocatorSuite : public ::testing::Test {
protected:
    void SetUp() override {
        using namespace NRdmaTest;
        if (IsRdmaTestDisabled()) {
            GTestSkip();
        }
    }
};

TEST_F(TAllocatorSuite, SlotPoolLimit) {
    const NInterconnect::NRdma::TMemPoolSettings settings {
        .SizeLimitMb = 32
    };
    static auto pool = NInterconnect::NRdma::CreateSlotMemPool(nullptr, settings);

    const size_t sz = 4 << 20;
    std::vector<NInterconnect::NRdma::TMemRegionPtr> regions;
    regions.reserve(8);
    size_t i = 0;
    for (;;i++) {
        auto reg = pool->Alloc(sz, 0);
        if (!reg) {
            UNIT_ASSERT(i == 8); // 32 / 4
            break;
        }
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
        ASSERT_TRUE(reg->GetSize() == sz) << "invalid size of allocated chunk";
        regions.push_back(reg);
    }

    regions.erase(regions.begin()); // free one region
return;
    {
        auto reg = pool->Alloc(sz, 0); // allocate one
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
        ASSERT_TRUE(reg->GetSize() == sz) << "invalid size of allocated chunk";
        UNIT_ASSERT(!pool->Alloc(sz, 0)); // pool is full
    }

    regions.clear();
}

TEST_F(TAllocatorSuite, SlotPoolHugeAlloc) {
    const NInterconnect::NRdma::TMemPoolSettings settings {
        .SizeLimitMb = 32
    };

    static auto pool = NInterconnect::NRdma::CreateSlotMemPool(nullptr, settings);

    std::vector<NInterconnect::NRdma::TMemRegionPtr> regions;
    const size_t sz = 8 << 20;
    for (size_t i = 0; i < 4; i++) {
        auto reg = pool->Alloc(sz, 0);
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
        regions.push_back(reg);
    }
    regions.clear();
}

TEST_F(TAllocatorSuite, SlotPoolHugeAllocAfterSmall) {
    const NInterconnect::NRdma::TMemPoolSettings settings {
        .SizeLimitMb = 32
    };

    const size_t smallSz = 1 << 20;
    const size_t hugeSz = 4 << 20;

   static auto pool = NInterconnect::NRdma::CreateSlotMemPool(nullptr, settings);

    std::vector<NInterconnect::NRdma::TMemRegionPtr> regions;
    regions.reserve(32);
    for (size_t i = 0; i < 32;i++) {
        auto reg = pool->Alloc(smallSz, 0);
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
        regions.push_back(reg);
    }
    regions.clear();

    auto reg = pool->Alloc(hugeSz, 0);
    ASSERT_TRUE(reg) << "allocation failed";
    ASSERT_TRUE(reg->GetAddr()) << "invalid address";
}

TEST_F(TAllocatorSuite, SlotPoolHugeAllocOtherThreadAfterSmall) {
    const NInterconnect::NRdma::TMemPoolSettings settings {
        .SizeLimitMb = 32
    };

    const size_t smallSz = 1 << 20;
    const size_t hugeSz = 4 << 20;

    static auto pool = NInterconnect::NRdma::CreateSlotMemPool(nullptr, settings);

    std::vector<NInterconnect::NRdma::TMemRegionPtr> regions;
    regions.reserve(32);
    for (size_t i = 0; i < 32;i++) {
        auto reg = pool->Alloc(smallSz, 0);
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
        regions.push_back(reg);
    }
    regions.clear();

    auto fn = [&]() {
        auto reg = pool->Alloc(hugeSz, 0);
        ASSERT_TRUE(reg) << "allocation failed";
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
    };

    std::thread thread(fn);
    thread.join();

    // And try to alloc small again
    for (size_t i = 0; i < 32;i++) {
        auto reg = pool->Alloc(smallSz, 0);
        ASSERT_TRUE(reg->GetAddr()) << "invalid address";
        regions.push_back(reg);
    }
    regions.clear();
}

TEST_F(TAllocatorSuite, AllocationWithReclaimOneThread) {
    const NInterconnect::NRdma::TMemPoolSettings settings {
        .SizeLimitMb = 32
    };

    static auto pool = NInterconnect::NRdma::CreateSlotMemPool(nullptr, settings);

    const ui32 NUM_ALLOC = 40000;

    TReallyFastRng32 rng(RandomNumber<ui64>());

    auto now = TInstant::Now();
    for (ui32 j = 0; j < NUM_ALLOC; ++j) {
        auto memRegion = pool->Alloc((rng() % 4096), 0);
        ASSERT_TRUE(memRegion->GetAddr()) << "invalid address";
    }

    float s = (TInstant::Now() - now).MicroSeconds();

    s = s / float(NUM_ALLOC);
    Cerr << "Average time per allocation: " << s << " us" << Endl;
}
