#include "mem_pool.h"
#include "link_manager.h"
#include "ctx.h"

#include <ydb/library/actors/interconnect/rdma/ibdrv/include/infiniband/verbs.h>

#include <util/thread/lfstack.h>
#include <util/stream/output.h>
#include <util/system/align.h>

#include <vector>

#include <unistd.h>
#include <sys/syscall.h>
#include <mutex>

namespace NInterconnect::NRdma {

    class TChunk: public NNonCopyable::TMoveOnly, public TAtomicRefCount<TChunk> {
    public:

    TChunk(std::vector<ibv_mr*>&& mrs, std::weak_ptr<IMemPool> pool, void* auxData) noexcept
        : MRs(std::move(mrs))
        , MemPool(pool)
        , AuxData(auxData)
    {
    }

    ~TChunk() {
        if (Empty()) {
            return;
        }
        auto addr = MRs.front()->addr;
        for (auto& m: MRs) {
            ibv_dereg_mr(m);
        }
        Cerr << "~TChunk: " << addr << Endl;
        std::free(addr);
        MRs.clear();
    }

    ibv_mr* GetMr(size_t deviceIndex) noexcept {
        if (Y_UNLIKELY(deviceIndex >= MRs.size())) {
            return nullptr;
        }
        return MRs[deviceIndex];
    }

    void Free(TMemRegion&& mr) noexcept {
        if (auto memPool = MemPool.lock()) {
            memPool->Free(std::move(mr), *this);
        }
    }

    bool Empty() const noexcept {
        return MRs.empty();
    }
    
    void* GetAuxData() noexcept {
        return AuxData;
    }

    private:
        std::vector<ibv_mr*> MRs;
        std::weak_ptr<IMemPool> MemPool;
        void* AuxData;
    };

    TMemRegion::TMemRegion(TChunkPtr chunk, uint32_t offset, uint32_t size) noexcept 
        : Chunk(std::move(chunk))
        , Offset(offset)
        , Size(size)
    {
        Y_ABORT_UNLESS(Chunk);
        Y_ABORT_UNLESS(!Chunk->Empty(), "Chunk is empty");
    }

    TMemRegion::~TMemRegion() {
        Chunk->Free(std::move(*this));
    }

    void* TMemRegion::GetAddr() const {
        auto* mr = Chunk->GetMr(0);
        if (Y_UNLIKELY(!mr)) {
            return nullptr;
        }
        return static_cast<char*>(mr->addr) + Offset;
    }
    uint32_t TMemRegion::GetSize() const {
        return Size;
    }

    uint32_t TMemRegion::GetLKey(size_t deviceIndex) const {
        auto* mr = Chunk->GetMr(deviceIndex);
        if (Y_UNLIKELY(!mr)) {
            return 0;
        }
        return mr->lkey;
    }
    uint32_t TMemRegion::GetRKey(size_t deviceIndex) const {
        auto* mr = Chunk->GetMr(deviceIndex);
        if (Y_UNLIKELY(!mr)) {
            return 0;
        }
        return mr->rkey;
    }

    TContiguousSpan TMemRegion::GetData() const {
        return TContiguousSpan(static_cast<const char*>(GetAddr()), GetSize());
    }
    TMutableContiguousSpan TMemRegion::GetDataMut() {
        return TMutableContiguousSpan(static_cast<char*>(GetAddr()), GetSize());
    }
    size_t TMemRegion::GetOccupiedMemorySize() const {
        return GetSize();
    }
    IContiguousChunk::EInnerType TMemRegion::GetInnerType() const noexcept {
        return EInnerType::RDMA_MEM_REG;
    }

    TMemRegionSlice::TMemRegionSlice(TIntrusivePtr<TMemRegion> memRegion, uint32_t offset, uint32_t size) noexcept
        : MemRegion(std::move(memRegion))
        , Offset(offset)
        , Size(size)
    {
        Y_ABORT_UNLESS(MemRegion);
        Y_ABORT_UNLESS(Offset + Size <= MemRegion->GetSize(), "Invalid slice size or offset");
    }

    void* TMemRegionSlice::GetAddr() const {
        return static_cast<char*>(MemRegion->GetAddr()) + Offset;
    }
    uint32_t TMemRegionSlice::GetSize() const {
        return Size;
    }

    uint32_t TMemRegionSlice::GetLKey(size_t deviceIndex) const {
        return MemRegion->GetLKey(deviceIndex);
    }
    uint32_t TMemRegionSlice::GetRKey(size_t deviceIndex) const {
        return MemRegion->GetRKey(deviceIndex);
    }

    TMemRegionSlice TryExtractFromRcBuf(const TRcBuf& rcBuf) noexcept {
        std::optional<IContiguousChunk::TPtr> underlying = rcBuf.ExtractFullUnderlyingContainer<IContiguousChunk::TPtr>();
        if (!underlying || !*underlying || underlying->Get()->GetInnerType() != IContiguousChunk::EInnerType::RDMA_MEM_REG) {
            return {};
        }
        auto memReg = dynamic_cast<NInterconnect::NRdma::TMemRegion*>(underlying->Get());
        if (!memReg) {
            return {};
        }
        return TMemRegionSlice(
            TIntrusivePtr<TMemRegion>(memReg),
            rcBuf.GetData() - memReg->GetData().data(),
            rcBuf.GetSize()
        );
    }

    void* allocateMemory(size_t size, size_t alignment) {
        if (size % alignment != 0) {
            return nullptr;
        }
        return std::aligned_alloc(alignment, size);
    }

    std::vector<ibv_mr*> registerMemory(void* addr, size_t size, const NInterconnect::NRdma::NLinkMgr::TCtxsMap& ctxs) {
        std::vector<ibv_mr*> res;
        res.reserve(ctxs.size());
        for (const auto& [_, ctx]: ctxs) {
            ibv_mr* mr = ibv_reg_mr(
                ctx->GetProtDomain(), addr, size,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
            );
            if (!mr) {
                std::free(addr);
                return {};
            }
            res.push_back(mr);
        }
        return res;
    }

    TMemRegionPtr IMemPool::Alloc(int size) noexcept {
        TMemRegion* region = nullptr;
        while (!region)
            region = AllocImpl(size);
        return TMemRegionPtr(region);
    }

    TRcBuf IMemPool::AllocRcBuf(int size) noexcept {
        return TRcBuf(IContiguousChunk::TPtr(AllocImpl(size)));
    }

    class TMemPoolBase: public IMemPool, public std::enable_shared_from_this<TMemPoolBase> {
    public:
        TMemPoolBase()
            : Ctxs(NInterconnect::NRdma::NLinkMgr::GetAllCtxs())
            , Alignment(NSystemInfo::GetPageSize())
        {
        }
    protected:
        template<typename TAuxData>
        TChunkPtr AllocNewChunk(size_t size) noexcept {
            size += sizeof(TAuxData);
            size = AlignUp(size, Alignment);
            void* ptr = allocateMemory(size, Alignment);
            if (!ptr) {
                return nullptr;
            }
            Cerr << "allocated: " << ptr << Endl;
            void* auxPtr = new((char*)ptr + (size - sizeof(TAuxData))) TAuxData;
            auto mrs = registerMemory(ptr, size, Ctxs);
            if (mrs.empty()) {
                Cerr << "Unable to register" << Endl;
                Y_ABORT_UNLESS(false, "UNABLE To REGISTER");
                return nullptr;
            }
            return MakeIntrusive<TChunk>(std::move(mrs), shared_from_this(), auxPtr);
        }

        const NInterconnect::NRdma::NLinkMgr::TCtxsMap Ctxs;
        size_t Alignment;
    };

    class TDummyMemPool: public TMemPoolBase {
    public:
        using TMemPoolBase::TMemPoolBase;

        TMemRegion* AllocImpl(int size) noexcept override {
            struct TDummy {};
            auto chunk = AllocNewChunk<TDummy>(size);
            if (!chunk) {
                return nullptr; 
            }
            return new TMemRegion(chunk , 0, size);
        }

        void Free(TMemRegion&&, TChunk&) noexcept override {}

        int GetMaxAllocSz() const noexcept override {
            return 2048 << 20;
        }
    };

    class TIncrementalMemPool: public TMemPoolBase {
    public:
        TIncrementalMemPool() {
            for (auto& x : ActiveAndFree) {
                x.store(nullptr);
            }
            for (auto& x : Inactive) {
                x.store(nullptr);
            }
        }

        struct TAuxChunkData {
            std::atomic<ui32> Allocated = 0; //not atomic modified only from alloc while not in shared array
            std::atomic<int> Freed;
            std::atomic<int> InactivePos = -1;
            bool IsInactive() const noexcept {
                return InactivePos.load(std::memory_order_acquire) >= 0;
            }
        };

        TMemRegion* AllocImpl(int size) noexcept override {
            if (size > (int)ChunkSize)
                return nullptr;

            size_t startPos = GetStartPos();
            size_t attempt = 7;
            TChunkPtr chunk;
            while (attempt--) {
                TChunk* cur = PopChunk(startPos, ActiveAndFree);
                if (!cur) {
                    // No chunks at all
                    break;
                }
                TAuxChunkData* aux = CastToAuxChunkData(cur);

                //Cerr << "aux:" << aux->InactivePos.load() << " " << (void*)cur << Endl;
                // We have chunk, check can we use it to allock region
                if (aux->Allocated.load() + (size_t)size > ChunkSize) {
                    Y_ABORT_UNLESS(!aux->IsInactive());
                    // No more space - put chunk in to inactive to be freed
                    int pos = PushChunk(startPos, Inactive, cur);
                    //Cerr << "No space: " <<  aux->Allocated.load() << "+ " <<  (size_t)size << " " << ChunkSize << " " << (void*)cur << " pos: " << pos << Endl;
                    if (pos >= 0) {
                        aux->InactivePos.store(pos);
                    } else {
                        cur->UnRef();
                    }
            
                    ReclaimInactive(0);
                } else {
                    //Cerr << "success: " << aux->Allocated.load() << " " << (void*)cur << Endl; 
                    Y_ABORT_UNLESS(!aux->IsInactive());
                    chunk = cur;
                    Y_ABORT_UNLESS(!CastToAuxChunkData(chunk.Get())->IsInactive());
                    break;
                }
            }
            if (chunk) {
                Y_ABORT_UNLESS(!CastToAuxChunkData(chunk.Get())->IsInactive());
            }
            if (chunk) {
                Y_ABORT_UNLESS(!CastToAuxChunkData(chunk.Get())->IsInactive());
            }
            if (!chunk) {
                chunk = AllocNewChunk<TAuxChunkData>(size);
                if (!chunk) {
                    return nullptr; 
                }
                chunk->Ref();
                Y_ABORT_UNLESS(!CastToAuxChunkData(chunk.Get())->IsInactive());
            }

            Y_ABORT_UNLESS(!CastToAuxChunkData(chunk.Get())->IsInactive());
            CastToAuxChunkData(chunk.Get())->Allocated.fetch_add(size);
            Y_ABORT_UNLESS(!CastToAuxChunkData(chunk.Get())->IsInactive());

            int ret = PushChunk(startPos, ActiveAndFree, chunk.Get());
            if (ret == -1) {
                chunk->UnRef();
            }
            return new TMemRegion(chunk , 0, size);
        }

        void Free(TMemRegion&&, TChunk& chunk) noexcept override {
            //Cerr << chunk.RefCount() << Endl;
            TAuxChunkData* auxData = CastToAuxChunkData(&chunk);
            if (auxData->IsInactive() && chunk.RefCount() == (1 + 1)) { // last MemRegion for chunk: 1 ref from TMemRegion and 1 is "manual" during allocation 
                Y_ABORT_UNLESS(auxData->InactivePos < (int)Inactive.size());
                Cerr << chunk.RefCount() << " " <<  auxData->InactivePos.load() << (void*)Inactive[auxData->InactivePos].load() << " " << (void*)&chunk<< Endl;
                Y_ABORT_UNLESS(Inactive[auxData->InactivePos].load() == &chunk);
                Inactive[auxData->InactivePos].store(nullptr);
                auxData->Allocated.store(0);
                auxData->InactivePos.store(-1);
                int ret = PushChunk(0, ActiveAndFree, &chunk);
                Cerr << "PushChunk: " << ret << Endl;
                if (ret == -1) {
                    chunk.UnRef();
                }
            }
        }

        int GetMaxAllocSz() const noexcept override {
            return ChunkSize;
        }

    private:
        static constexpr size_t ChunkSize = 16 << 20;
        static constexpr size_t MaxChunks = 1 << 4; //must be power of two
        static constexpr size_t CacheLineSz = 64;
        static constexpr size_t ChunkGap = CacheLineSz / sizeof(TChunk*); 

        using TChunkContainer = std::array<std::atomic<TChunk*>, MaxChunks>; 

        static size_t WrapPos(size_t x) noexcept {
            return x % MaxChunks;
        }

        static size_t GetStartPos() noexcept {
            size_t id = syscall(SYS_gettid);
            return (id + ChunkGap) % MaxChunks;

        }

        static TAuxChunkData* CastToAuxChunkData(TChunk* chunk) noexcept {
            return reinterpret_cast<TAuxChunkData*>(chunk->GetAuxData());
        }

        static TChunk* PopChunk(size_t startPos, TChunkContainer& cont) noexcept {
            for (size_t i = 0, j = startPos; i < MaxChunks; i++, j++) {
                size_t pos = WrapPos(j);
                TChunk* p = cont[pos].exchange(nullptr, std::memory_order_seq_cst);
                if (p) {
                    return p;
                }

/*                
                TChunk* p = cont[pos].load(std::memory_order_relaxed);
                if (p == nullptr) {
                    continue;
                }
                if (cont[pos].compare_exchange_strong(p, nullptr, std::memory_order_seq_cst)) {
                    return p;
                }
                    */
            }
            return nullptr;
        }

        static int PushChunk(size_t startPos, TChunkContainer& cont, TChunk* chunk) noexcept {
            for (size_t i = 0, j = startPos; i < MaxChunks; i++, j++) {
                size_t pos = WrapPos(j);
                TChunk* p = cont[pos].load(std::memory_order_relaxed);
                if (p != nullptr) {
                    continue;
                }
                if (cont[pos].compare_exchange_strong(p, chunk, std::memory_order_seq_cst)) {
                    return pos;
                }
            }
            return -1;
        }

        void ReclaimInactive(size_t startPos) noexcept {
            if (!Mutex.try_lock()) {
                return;
            }
            for (size_t i = 0, j = startPos; i < MaxChunks; i++, j++) {
                size_t pos = WrapPos(j);
                TChunk* p = Inactive[pos].load(std::memory_order_seq_cst);
                if (p == nullptr || !CastToAuxChunkData(p)->IsInactive()) {
                    continue;
                }
                if (p->RefCount() == 1) {
                    if (Inactive[pos].compare_exchange_strong(p, nullptr, std::memory_order_seq_cst)) {
                        if (!CastToAuxChunkData(p)->IsInactive()) {
                            //if (PushChunk(0, Inactive, p) == -1) {
                            //    Y_ABORT_UNLESS(expr, ...)
                           // }
                            continue;
                        }
                        auto aux = CastToAuxChunkData(p);
                        aux->Allocated.store(0);
                        aux->InactivePos.store(-1);

                        
                        auto pos = PushChunk(startPos, ActiveAndFree, p);
                        if (pos == -1) {
                            Cerr << "unable to reclaim" << Endl;
                            p->UnRef();
                        } else {
                            //Cerr << "Reclaim success: " << pos << " " << (void*)p<< Endl;
                        }
                        //return;
                    }
                }
            }
            Mutex.unlock();
        }
        
        TChunkContainer ActiveAndFree;
        TChunkContainer Inactive; 
        std::mutex Mutex;
    };

    std::shared_ptr<IMemPool> CreateDummyMemPool() noexcept {
        return std::make_shared<TDummyMemPool>();
    }

    std::shared_ptr<IMemPool> CreateIncrementalMemPool() noexcept {
        return std::make_shared<TIncrementalMemPool>();
    }
}
