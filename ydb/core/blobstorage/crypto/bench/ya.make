Y_BENCHMARK()

BENCHMARK_OPTS(--budget=10)

SRCS(
    main.cpp
)

PEERDIR(
    ydb/core/blobstorage/crypto
)

END()
