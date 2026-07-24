/*
Copyright (c) 2026 acrion innovations GmbH

Transport-neutral, content-verified object materialisation for partial queries.
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace zelph::network
{
    enum class CacheDisposition
    {
        local,
        verified_hit,
        fetched,
    };

    struct ObjectRequest
    {
        std::string dataset_version;
        std::string uri;
        std::string revision;
        std::string expected_sha256;
        std::string section_hint;
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    struct VerifiedObject
    {
        std::filesystem::path local_path;
        uint64_t byte_size = 0;
        CacheDisposition cache_disposition = CacheDisposition::local;
        uint64_t materialize_milliseconds = 0;
        uint64_t verify_milliseconds = 0;
        std::string sha256;
    };

    class VerifiedObjectStore
    {
    public:
        VerifiedObject materialize(const ObjectRequest& request) const;
        void enforce_quota() const;

        static const char* disposition_name(CacheDisposition disposition);
    };
} // namespace zelph::network
