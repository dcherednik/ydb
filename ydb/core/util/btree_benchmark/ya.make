Y_BENCHMARK()

TAG(ya:fat)
SIZE(LARGE)

ALLOCATOR(LF)

PEERDIR(
    library/cpp/threading/skip_list
    ydb/core/util
)

SRCS(
    main.cpp
)

END()
