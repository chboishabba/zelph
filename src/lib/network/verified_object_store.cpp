/*
Copyright (c) 2026 acrion innovations GmbH
*/

#include "verified_object_store.hpp"

#ifndef __EMSCRIPTEN__
#include "manifest_loader.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace zelph::network
{
    namespace
    {
        std::mutex flight_map_mutex;
        std::map<std::string, std::weak_ptr<std::mutex>> flight_mutexes;

        uint64_t millis_since(const std::chrono::steady_clock::time_point start)
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        }

        bool is_remote(const std::string& uri)
        {
            return detail::is_hf_uri(uri);
        }

        std::filesystem::path local_path_for(const std::string& uri)
        {
            if (uri.rfind("file://", 0) == 0) return std::filesystem::path(uri.substr(7));
            return std::filesystem::path(uri);
        }

        std::filesystem::path verified_root()
        {
            const char* configured = std::getenv("ZELPH_HF_CACHE_DIR");
            std::filesystem::path root = configured && *configured
                                       ? std::filesystem::path(configured) / "verified-v1"
                                       : std::filesystem::temp_directory_path() / "zelph-hf-cache" / "verified-v1";
            std::filesystem::create_directories(root);
            return root;
        }

        std::string request_identity(const ObjectRequest& request)
        {
            return request.dataset_version + "\n" + request.uri + "\n" + request.revision + "\n"
                 + std::to_string(request.offset) + "\n" + std::to_string(request.length) + "\n"
                 + sha256::normalize(request.expected_sha256);
        }

        std::shared_ptr<std::mutex> flight_mutex(const std::string& identity)
        {
            std::lock_guard lock(flight_map_mutex);
            auto& weak = flight_mutexes[identity];
            auto result = weak.lock();
            if (!result)
            {
                result = std::make_shared<std::mutex>();
                weak = result;
            }
            return result;
        }

        uint64_t file_size(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error) throw std::runtime_error("Cannot stat partial-query object " + path.string() + ": " + error.message());
            return size;
        }

        std::string verify(const std::filesystem::path& path, const ObjectRequest& request)
        {
            const auto size = file_size(path);
            if (request.length && size != request.length)
                throw std::runtime_error("Partial-query object size mismatch for " + request.uri
                                         + ": expected " + std::to_string(request.length)
                                         + ", got " + std::to_string(size));
            const auto digest = sha256::file(path);
            const auto expected = sha256::normalize(request.expected_sha256);
            if (!expected.empty() && digest != expected)
                throw std::runtime_error("Partial-query object SHA-256 mismatch for " + request.uri
                                         + ": expected " + expected + ", got " + digest);
            return digest;
        }

        void copy_atomically(const std::filesystem::path& from, const std::filesystem::path& to)
        {
            const auto temporary = std::filesystem::path(to.string() + ".tmp");
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            std::filesystem::copy_file(from, temporary, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::rename(temporary, to, ignored);
            if (ignored)
            {
                std::filesystem::remove(to, ignored);
                ignored.clear();
                std::filesystem::rename(temporary, to, ignored);
                if (ignored) throw std::runtime_error("Cannot publish verified cache object " + to.string());
            }
        }
    }

    const char* VerifiedObjectStore::disposition_name(const CacheDisposition disposition)
    {
        switch (disposition)
        {
        case CacheDisposition::local: return "local";
        case CacheDisposition::verified_hit: return "verified-hit";
        case CacheDisposition::fetched: return "fetched";
        }
        return "unknown";
    }

    VerifiedObject VerifiedObjectStore::materialize(const ObjectRequest& request) const
    {
        if (request.uri.empty()) throw std::runtime_error("Partial-query object URI is empty");
        const auto started = std::chrono::steady_clock::now();
        const auto identity = request_identity(request);
        const auto guard = flight_mutex(identity);
        std::lock_guard flight_lock(*guard);

        VerifiedObject result;
        if (!is_remote(request.uri))
        {
            result.local_path = std::filesystem::absolute(local_path_for(request.uri));
            if (!std::filesystem::is_regular_file(result.local_path))
                throw std::runtime_error("Partial-query local object not found: " + result.local_path.string());
            const auto verify_started = std::chrono::steady_clock::now();
            result.sha256 = verify(result.local_path, request);
            result.verify_milliseconds = millis_since(verify_started);
            result.byte_size = file_size(result.local_path);
            result.cache_disposition = CacheDisposition::local;
            result.materialize_milliseconds = millis_since(started);
            return result;
        }

        const auto cache_path = verified_root() / (sha256::bytes(identity) + ".body");
        if (std::filesystem::is_regular_file(cache_path))
        {
            try
            {
                const auto verify_started = std::chrono::steady_clock::now();
                result.sha256 = verify(cache_path, request);
                result.verify_milliseconds = millis_since(verify_started);
                result.local_path = cache_path;
                result.byte_size = file_size(cache_path);
                result.cache_disposition = CacheDisposition::verified_hit;
                result.materialize_milliseconds = millis_since(started);
                std::error_code ignored;
                std::filesystem::last_write_time(cache_path, std::filesystem::file_time_type::clock::now(), ignored);
                return result;
            }
            catch (...)
            {
                std::error_code ignored;
                std::filesystem::remove(cache_path, ignored);
            }
        }

        const auto fetched = detail::fetch_chunk_to_cache(
            request.uri, request.offset, request.length,
            request.section_hint.empty() ? "partial-query" : request.section_hint);
        const auto verify_started = std::chrono::steady_clock::now();
        result.sha256 = verify(fetched, request);
        result.verify_milliseconds = millis_since(verify_started);
        copy_atomically(fetched, cache_path);
        result.local_path = cache_path;
        result.byte_size = file_size(cache_path);
        result.cache_disposition = CacheDisposition::fetched;
        result.materialize_milliseconds = millis_since(started);
        enforce_quota();
        return result;
    }

    void VerifiedObjectStore::enforce_quota() const
    {
        const char* configured = std::getenv("ZELPH_HF_CACHE_MAX_BYTES");
        if (!configured || !*configured) return;
        uint64_t limit = 0;
        try { limit = std::stoull(configured); }
        catch (...) { return; }
        if (!limit) return;

        struct Entry { std::filesystem::path path; uint64_t size; std::filesystem::file_time_type time; };
        std::vector<Entry> entries;
        uint64_t total = 0;
        for (const auto& item : std::filesystem::directory_iterator(verified_root()))
        {
            if (!item.is_regular_file() || item.path().extension() != ".body") continue;
            const uint64_t size = file_size(item.path());
            total += size;
            entries.push_back({item.path(), size, item.last_write_time()});
        }
        if (total <= limit) return;
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
        for (const auto& entry : entries)
        {
            if (total <= limit) break;
            std::error_code ignored;
            if (std::filesystem::remove(entry.path, ignored)) total -= entry.size;
        }
    }
} // namespace zelph::network
#endif
