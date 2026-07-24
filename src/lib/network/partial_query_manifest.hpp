/*
Copyright (c) 2026 acrion innovations GmbH

Validated hosted-artifact contract for routed partial queries.
*/
#pragma once

#ifndef __EMSCRIPTEN__
#include "manifest_loader.hpp"
#endif

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace zelph::network
{
    enum class PartialChunkSection { left, right, name_of_node, node_of_name };

    struct PartialChunkDescriptor
    {
        std::string id;
        PartialChunkSection section = PartialChunkSection::left;
        uint32_t chunk_index = 0;
        uint64_t byte_size = 0;
        uint64_t source_offset = 0;
        bool has_source_offset = false;
        std::string uri;
        std::string sha256;
        std::string layer;
    };

    struct PartialRoutingIndexDescriptor
    {
        std::string id;
        std::string key;
        std::string uri;
        std::string sha256;
        std::string format_version;
        bool exhaustive = false;
    };

    class PartialQueryManifest
    {
    public:
#ifndef __EMSCRIPTEN__
        static PartialQueryManifest load(const std::string& source, const std::string& shard_root = {});
        const detail::ManifestDescription& legacy() const { return _legacy; }
        detail::ManifestDescription& legacy() { return _legacy; }
#endif
        bool canonical() const { return _canonical; }
        bool certifiable() const;
        bool layer_available(const std::string& layer) const;
        bool global_coverage() const { return _global_coverage; }
        const std::string& local_path() const { return _local_path; }
        const std::string& source() const { return _source; }
        const std::string& dataset_id() const { return _dataset_id; }
        const std::string& dataset_version() const { return _dataset_version; }
        const std::string& manifest_sha256() const { return _manifest_sha256; }
        const PartialRoutingIndexDescriptor& node_routing_index() const { return _node_routing_index; }

        const PartialChunkDescriptor* chunk(PartialChunkSection section, uint32_t index) const;
        std::vector<PartialChunkDescriptor> chunks(PartialChunkSection section) const;
        void add_chunk(PartialChunkDescriptor descriptor);
        std::string coverage_summary() const;

        static const char* section_name(PartialChunkSection section);
        static PartialChunkSection parse_section(const std::string& section);

    private:
#ifndef __EMSCRIPTEN__
        detail::ManifestDescription _legacy;
#endif
        bool _canonical = false;
        bool _manifest_hash_valid = false;
        bool _global_coverage = false;
        std::string _source;
        std::string _local_path;
        std::string _dataset_id;
        std::string _dataset_version;
        std::string _manifest_sha256;
        std::set<std::string> _layers;
        PartialRoutingIndexDescriptor _node_routing_index;
        std::map<std::pair<PartialChunkSection, uint32_t>, PartialChunkDescriptor> _chunks;
    };
} // namespace zelph::network
