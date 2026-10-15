// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "common/types.h"

namespace Core::Downloads {

struct Url {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;

    std::string Origin() const;
};

std::optional<Url> ParseUrl(const std::string& url);

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;

    bool Ok() const {
        return error.empty() && status >= 200 && status < 300;
    }
};

// Called with each chunk of the response body. Return false to abort the transfer.
using ChunkSink = std::function<bool(std::span<const char>)>;
// Called with (received, total); total is 0 when the server didn't send a length.
using ProgressSink = std::function<void(u64, u64)>;

HttpResponse Get(const std::string& url);

HttpResponse Stream(const std::string& url, const ChunkSink& sink, const ProgressSink& progress,
                    const std::atomic<bool>* cancel);

// Reuses one keep-alive connection for many requests to the same host.
class Session {
public:
    explicit Session(const std::string& origin);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    HttpResponse Get(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::Downloads
