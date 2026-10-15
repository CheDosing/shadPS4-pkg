// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <fmt/format.h>
#include <httplib.h>

#include "common/logging/log.h"
#include "core/downloads/http_client.h"

namespace Core::Downloads {

namespace {

constexpr auto UserAgent = "shadPS4-pkg";

std::string GetEnv(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool HostBypassesProxy(const std::string& host) {
    std::string no_proxy = GetEnv("NO_PROXY");
    if (no_proxy.empty()) {
        no_proxy = GetEnv("no_proxy");
    }
    const std::string lower_host = ToLower(host);
    size_t start = 0;
    while (start <= no_proxy.size()) {
        const size_t end = std::min(no_proxy.find(',', start), no_proxy.size());
        std::string entry = ToLower(no_proxy.substr(start, end - start));
        entry.erase(0, entry.find_first_not_of(" \t"));
        entry.erase(entry.find_last_not_of(" \t") + 1);
        if (!entry.empty()) {
            if (entry == "*") {
                return true;
            }
            if (entry.front() == '.') {
                entry.erase(0, 1);
            }
            if (lower_host == entry ||
                (lower_host.size() > entry.size() && lower_host.ends_with("." + entry))) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

std::optional<Url> ProxyFor(const Url& target) {
    if (HostBypassesProxy(target.host)) {
        return std::nullopt;
    }
    std::string proxy = target.scheme == "https" ? GetEnv("HTTPS_PROXY") : GetEnv("HTTP_PROXY");
    if (proxy.empty()) {
        proxy = target.scheme == "https" ? GetEnv("https_proxy") : GetEnv("http_proxy");
    }
    if (proxy.empty()) {
        return std::nullopt;
    }
    if (proxy.find("://") == std::string::npos) {
        proxy = "http://" + proxy;
    }
    return ParseUrl(proxy);
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
std::string FindCaBundle() {
    if (auto from_env = GetEnv("SSL_CERT_FILE"); !from_env.empty()) {
        return from_env;
    }
#ifndef _WIN32
    static constexpr const char* candidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",     // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",       // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                 // openSUSE
        "/etc/pki/tls/cacert.pem",                // OpenELEC
        "/etc/ssl/cert.pem",                      // macOS, Alpine, FreeBSD
        "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD
    };
    std::error_code ec;
    for (const char* path : candidates) {
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
    }
#endif
    return {};
}
#endif

std::unique_ptr<httplib::Client> MakeClient(const Url& url) {
    auto client = std::make_unique<httplib::Client>(url.Origin());
    client->set_follow_location(true);
    client->set_connection_timeout(20);
    client->set_read_timeout(60);
    client->set_write_timeout(30);
    client->set_default_headers({{"User-Agent", UserAgent}});
    if (const auto proxy = ProxyFor(url)) {
        client->set_proxy(proxy->host, proxy->port);
    }
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (url.scheme == "https") {
        if (const auto bundle = FindCaBundle(); !bundle.empty()) {
            client->set_ca_cert_path(bundle);
        }
    }
#endif
    return client;
}

std::string DescribeError(httplib::Error error) {
    return httplib::to_string(error);
}

} // namespace

std::string Url::Origin() const {
    const bool default_port =
        (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
    return default_port ? fmt::format("{}://{}", scheme, host)
                        : fmt::format("{}://{}:{}", scheme, host, port);
}

std::optional<Url> ParseUrl(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return std::nullopt;
    }
    Url out;
    out.scheme = ToLower(url.substr(0, scheme_end));
    if (out.scheme != "http" && out.scheme != "https") {
        return std::nullopt;
    }
    const auto host_start = scheme_end + 3;
    const auto path_start = url.find('/', host_start);
    std::string authority = url.substr(
        host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    out.path = path_start == std::string::npos ? "/" : url.substr(path_start);

    if (const auto at = authority.rfind('@'); at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    out.port = out.scheme == "https" ? 443 : 80;
    if (const auto colon = authority.rfind(':');
        colon != std::string::npos && authority.find(']') == std::string::npos) {
        try {
            out.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return std::nullopt;
        }
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) {
        return std::nullopt;
    }
    out.host = authority;
    return out;
}

HttpResponse Get(const std::string& url) {
    std::string body;
    auto response = Stream(
        url,
        [&](std::span<const char> chunk) {
            body.append(chunk.data(), chunk.size());
            return true;
        },
        nullptr, nullptr);
    response.body = std::move(body);
    return response;
}

HttpResponse Stream(const std::string& url, const ChunkSink& sink, const ProgressSink& progress,
                    const std::atomic<bool>* cancel) {
    HttpResponse out;
    const auto parsed = ParseUrl(url);
    if (!parsed) {
        out.error = "Invalid URL: " + url;
        return out;
    }

    auto client = MakeClient(*parsed);
    bool sink_aborted = false;
    const auto result = client->Get(
        parsed->path,
        [&](const httplib::Response& response) {
            out.status = response.status;
            return (response.status >= 200 && response.status < 400);
        },
        [&](const char* data, size_t length) {
            if (cancel && *cancel) {
                return false;
            }
            if (!sink({data, length})) {
                sink_aborted = true;
                return false;
            }
            return true;
        },
        [&](size_t current, size_t total) {
            if (progress) {
                progress(current, total);
            }
            return !(cancel && *cancel);
        });

    if (!result) {
        if (cancel && *cancel) {
            out.error = "Cancelled";
        } else if (sink_aborted) {
            out.error = "Could not save the downloaded data";
        } else if (out.status >= 400) {
            out.error = fmt::format("Server returned HTTP {}", out.status);
        } else {
            out.error = fmt::format("Connection failed: {}", DescribeError(result.error()));
        }
        return out;
    }
    out.status = result->status;
    if (out.status >= 400) {
        out.error = fmt::format("Server returned HTTP {}", out.status);
    }
    return out;
}

struct Session::Impl {
    std::unique_ptr<httplib::Client> client;
    std::string init_error;
};

Session::Session(const std::string& origin) : impl(std::make_unique<Impl>()) {
    if (const auto parsed = ParseUrl(origin)) {
        impl->client = MakeClient(*parsed);
        impl->client->set_keep_alive(true);
    } else {
        impl->init_error = "Invalid URL: " + origin;
    }
}

Session::~Session() = default;

HttpResponse Session::Get(const std::string& path) {
    HttpResponse out;
    if (!impl->client) {
        out.error = impl->init_error;
        return out;
    }
    const auto result = impl->client->Get(path);
    if (!result) {
        out.error = fmt::format("Connection failed: {}", DescribeError(result.error()));
        return out;
    }
    out.status = result->status;
    out.body = result->body;
    if (out.status >= 400) {
        out.error = fmt::format("Server returned HTTP {}", out.status);
    }
    return out;
}

} // namespace Core::Downloads
