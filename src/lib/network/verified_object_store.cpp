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
#include <sstream>
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

        bool env_true(const char* name)
        {
            const char* value = std::getenv(name);
            return value && (*value == '1' || std::string_view(value) == "true" || std::string_view(value) == "TRUE");
        }

        uint64_t env_u64(const char* name, uint64_t fallback)
        {
            const char* value = std::getenv(name);
            if (!value || !*value) return fallback;
            try { return std::stoull(value); }
            catch (...) { return fallback; }
        }

        bool is_remote(const std::string& uri) { return detail::is_hf_uri(uri); }

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

        uint64_t verified_file_size(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error) throw std::runtime_error("Cannot stat partial-query object " + path.string() + ": " + error.message());
            return size;
        }

        std::string verify(const std::filesystem::path& path, const ObjectRequest& request)
        {
            const auto size = verified_file_size(path);
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

        void publish_atomically(const std::filesystem::path& from, const std::filesystem::path& to)
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

        void extract_range(const std::filesystem::path& source,
                           const std::filesystem::path& destination,
                           const uint64_t offset,
                           const uint64_t length)
        {
            std::ifstream input(source, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open partial-query source: " + source.string());
            input.seekg(static_cast<std::streamoff>(offset));
            if (!input) throw std::runtime_error("Cannot seek partial-query source: " + source.string());

            const auto temporary = std::filesystem::path(destination.string() + ".tmp");
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot create partial-query range: " + temporary.string());
            std::array<char, 1 << 16> buffer{};
            uint64_t remaining = length;
            while (remaining > 0)
            {
                const auto take = static_cast<std::streamsize>(std::min<uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), take);
                if (input.gcount() != take)
                    throw std::runtime_error("Local partial-query range is truncated: " + source.string());
                output.write(buffer.data(), take);
                remaining -= static_cast<uint64_t>(take);
            }
            output.close();
            std::error_code ignored;
            std::filesystem::rename(temporary, destination, ignored);
            if (ignored)
            {
                std::filesystem::remove(destination, ignored);
                ignored.clear();
                std::filesystem::rename(temporary, destination, ignored);
                if (ignored) throw std::runtime_error("Cannot publish local partial-query range");
            }
        }

        bool negative_cache_active(const std::filesystem::path& path, std::string& reason)
        {
            if (!std::filesystem::is_regular_file(path)) return false;
            const uint64_t ttl = env_u64("ZELPH_HF_NEGATIVE_CACHE_SECONDS", 30);
            if (!ttl) return false;
            std::error_code error;
            const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path, error);
            if (error || age > std::chrono::seconds(ttl))
            {
                std::filesystem::remove(path, error);
                return false;
            }
            std::ifstream input(path);
            std::getline(input, reason);
            return true;
        }

        void write_negative_cache(const std::filesystem::path& path, const std::string& reason)
        {
            std::ofstream output(path, std::ios::trunc);
            if (output) output << reason << '\n';
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

        const auto cache_path = verified_root() / (sha256::bytes(identity) + ".body");
        const auto negative_path = verified_root() / (sha256::bytes(identity) + ".negative");
        std::string negative_reason;
        if (negative_cache_active(negative_path, negative_reason))
            throw std::runtime_error("Recently failed partial-query object: " + negative_reason);

        VerifiedObject result;
        if (std::filesystem::is_regular_file(cache_path))
        {
            try
            {
                const auto verify_started = std::chrono::steady_clock::now();
                result.sha256 = verify(cache_path, request);
                result.verify_milliseconds = millis_since(verify_started);
                result.local_path = cache_path;
                result.byte_size = verified_file_size(cache_path);
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

        if (env_true("ZELPH_HF_OFFLINE"))
            throw std::runtime_error("Offline mode has no verified cache object for " + request.uri);

        try
        {
            std::filesystem::path materialized;
            CacheDisposition disposition = CacheDisposition::fetched;
            if (!is_remote(request.uri))
            {
                const auto source = std::filesystem::absolute(local_path_for(request.uri));
                if (!std::filesystem::is_regular_file(source))
                    throw std::runtime_error("Partial-query local object not found: " + source.string());
                if (request.offset || request.length)
                {
                    if (!request.length) throw std::runtime_error("A local range requires an explicit length");
                    extract_range(source, cache_path, request.offset, request.length);
                    materialized = cache_path;
                    disposition = CacheDisposition::local;
                }
                else
                {
                    materialized = source;
                    disposition = CacheDisposition::local;
                }
            }
            else
            {
                materialized = detail::fetch_chunk_to_cache(
                    request.uri, request.offset, request.length,
                    request.section_hint.empty() ? "partial-query" : request.section_hint);
            }

            const auto verify_started = std::chrono::steady_clock::now();
            result.sha256 = verify(materialized, request);
            result.verify_milliseconds = millis_since(verify_started);
            if (materialized != cache_path) publish_atomically(materialized, cache_path);
            result.local_path = cache_path;
            result.byte_size = verified_file_size(cache_path);
            result.cache_disposition = disposition;
            result.materialize_milliseconds = millis_since(started);
            std::error_code ignored;
            std::filesystem::remove(negative_path, ignored);
            enforce_quota();
            return result;
        }
        catch (const std::exception& error)
        {
            write_negative_cache(negative_path, error.what());
            throw;
        }
    }

    void VerifiedObjectStore::enforce_quota() const
    {
        const uint64_t limit = env_u64("ZELPH_HF_CACHE_MAX_BYTES", 0);
        if (!limit) return;
        struct Entry { std::filesystem::path path; uint64_t size; std::filesystem::file_time_type time; };
        std::vector<Entry> entries;
        uint64_t total = 0;
        for (const auto& item : std::filesystem::directory_iterator(verified_root()))
        {
            if (!item.is_regular_file() || item.path().extension() != ".body") continue;
            const uint64_t size = verified_file_size(item.path());
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
