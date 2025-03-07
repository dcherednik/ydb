#include <sys/types.h>

#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>

#include "lib.h"

#include <util/datetime/base.h>
#include <util/stream/output.h>
#include <util/thread/pool.h>

#include <mutex>
#include <memory>
#include <deque>

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

// Provides some way to mock MSG_ZEROCOPY behavious on the same host


using TLsend =    ssize_t(*) (int fd, const void *buf, size_t len, int flags);
using TLrecvmsg = ssize_t(*) (int fd, struct msghdr *message, int flags);

static TLsend    lsend =    (TLsend)    dlsym(RTLD_NEXT, "send");
static TLrecvmsg lrecvmsg = (TLrecvmsg) dlsym(RTLD_NEXT, "recvmsg");

class TFdCtx {
public:
    TFdCtx(int fd, IThreadPool* pool)
        : Fd(fd)
        , Pool(pool)
    {}

    bool EmulateZcSend(const void *buf, size_t n) noexcept {
return false;
        class TZcSendTask : public IObjectInQueue {
        public:
            TZcSendTask(int fd, const void *buf, size_t n)
                : Fd(fd)
                , Buf(buf)
                , Size(n)
            {}
            void Process(void*) override {
                Cerr << "PROCESS" << Endl;
                size_t processed = 0;
                while (processed < Size) {
                    ssize_t rv = lsend(Fd, (const char*)Buf + processed, Size - processed, 0);
                    if (rv >= 0) {
                        processed += rv;
                        continue;
                    } else {
                        switch (errno) {
                            case EAGAIN:
                                Sleep(TDuration::MilliSeconds(1)); //Ok for tests
                            case EINTR:
                                continue;
                            default:
                                Y_ABORT("unexpected errno");
                        }
                    }
                }
            }
        private:
            const int Fd;
            const void *Buf;
            const size_t Size;
        };

        auto task = new TZcSendTask(Fd, buf, n);
        if (Pool->Add(task) ) {
            return true;
        } else {
            delete task;
            return false;
        }
    }
    
private:
    const int Fd;
    IThreadPool* Pool;
};

class TZcSendMock {
public:
    TZcSendMock()
    {
        Cerr << "TZcSendMock" << Endl;
    }
    TFdCtx* RegisterFd(int fd) noexcept {
        std::lock_guard<std::mutex> lock(Mutex);
        if (!Pool) {
            Pool.reset(new TAdaptiveThreadPool());
            Pool->Start(100);
        }
        auto [it, res] = Fds.try_emplace(fd, nullptr);
        if (res) {
            it->second = std::make_unique<TFdCtx>(fd, Pool.get());
        }
        return it->second.get();
    }
private:
    std::mutex Mutex;
    std::unordered_map<int, std::unique_ptr<TFdCtx>> Fds;
    std::unique_ptr<IThreadPool> Pool;
};

static TZcSendMock ZcSendMock;

extern "C" {

ssize_t send (int fd, const void *buf, size_t n, int flags) {
    auto ctx = ZcSendMock.RegisterFd(fd);
    Cerr << "Fd: " << fd << " Send " << n << " flags: " << flags << Endl;
    if (flags & MSG_ZEROCOPY) {
        if (ctx->EmulateZcSend(buf, n)) {
            return n;
        }
    }

    return lsend(fd, buf, n, flags);
}

ssize_t recvmsg (int fd, struct msghdr *message, int flags) {
    ZcSendMock.RegisterFd(fd); 
    return lrecvmsg(fd, message, flags);
}

}

