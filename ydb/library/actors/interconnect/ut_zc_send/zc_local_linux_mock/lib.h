#pragma once 

#include <functional>

using TSendCb = std::function<ssize_t (int sockfd, const void *buf, size_t len, int flags)>;
void RegisterSend(TSendCb&& sendCb);
