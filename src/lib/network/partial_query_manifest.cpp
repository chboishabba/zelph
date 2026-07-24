/*
Copyright (c) 2026 acrion innovations GmbH
*/

#include "partial_query_manifest.hpp"

#ifndef __EMSCRIPTEN__
#include "sha256.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace zelph::network
{
    namespace
    {
        std::string read_all(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open partial-query manifest: " + path.string());
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }

        template <typename Visitor>
        void for_each_object(std::string_view array_json, Visitor&& visitor)
        {
            if (array_json.size() < 2 || array_json.front() != '[') return;
            size_t cursor = 1;
            const size_t stop = array_json.size() - 1;
            while (cursor < stop)
            {
                cursor = detail::skip_json_ws(array_json, cursor);
                if (cursor >= stop || array_json[cursor] == ']') break;
                if (array_json[cursor] != '{') throw std::runtime_error("Expected object in partial-query manifest array");
                size_t next = 0;
                const auto object = detail::extract_balanced(array_json, cursor, '{', '}', next);
                if (object.empty()) throw std::runtime_error("Malformed object in partial-query manifest array");
                visitor(object);
                cursor = detail::skip_json_ws(array_json, next);
                if (cursor < stop && array_json[cursor] == ',') ++cursor;
            }
        }

        std::string digest_value(std::string_view object)
        {
            const auto digest = detail::find_json_object(object, "digest");
            std::string value;
            if (!digest.empty()) detail::parse_json_string_field(digest, "value", value);
            if (value.empty()) detail::parse_json_string_field(object, "sha256", value);
            return sha256::normalize(value);
        }

        std::string first_transport_uri(std::string_view object)
        {
            const auto transports = detail::find_json_array(object, "transports");
            std::string uri;
            for_each_object(transports, [&](std::string_view transport)
                            { if (uri.empty()) detail::parse_json_string_field(transport, "uri", uri); });
            if (uri.empty()) detail::parse_json_string_field(object, "objectPath", uri);
            return uri;
        }

        std::string first_layer(std::string_view object)
        {
            std::vector<std::string> layers;
            if (detail::parse_json_string_array_field(object, "layers", layers) && !layers.empty()) return layers.front();
            std::string layer;
            detail::parse_json_string_field(object, "layer", layer);
            return layer;
        }

        std::string blank_declared_manifest_hash(std::string text)
        {
            const auto key = text.find("\"manifestHash\"");
            if (key == std::string::npos) return text;
            const auto value_key = text.find("\"value\"", key);
            if (value_key == std::string::npos) return text;
            const auto colon = text.find(':', value_key + 7);
            if (colon == std::string::npos) return text;
            const auto quote = text.find('"', colon + 1);
            if (quote == std::string::npos) return text;
            const auto end = text.find('"', quote + 1);
            if (end == std::string::npos) return text;
            text.erase(quote + 1, end - quote - 1);
            return text;
        }

        void add_legacy_chunks(PartialQueryManifest& manifest,
                               PartialChunkSection section,
                               const detail::ManifestSection& legacy_section)
        {
            for (const auto& ref : legacy_section.chunks)
            {
                if (manifest.chunk(section, ref.chunk_index)) continue;
                PartialChunkDescriptor descriptor;
                descriptor.id = std::string(PartialQueryManifest::section_name(section)) + ":" + std::to_string(ref.chunk_index);
                descriptor.section = section;
                descriptor.chunk_index = ref.chunk_index;
                descriptor.byte_size = ref.length;
                descriptor.source_offset = ref.source_offset;
                descriptor.has_source_offset = ref.has_source_offset;
                descriptor.uri = ref.object_path;
                descriptor.layer = "directClaim";
                manifest.add_chunk(std::move(descriptor));
            }
        }
    }

    const char* PartialQueryManifest::section_name(const PartialChunkSection section)
    {
        switch (section)
        {
        case PartialChunkSection::left: return "left";
        case PartialChunkSection::right: return "right";
        case PartialChunkSection::name_of_node: return "nameOfNode";
        case PartialChunkSection::node_of_name: return "nodeOfName";
        }
        return "left";
    }

    PartialChunkSection PartialQueryManifest::parse_section(const std::string& section)
    {
        if (section == "left") return PartialChunkSection::left;
        if (section == "right") return PartialChunkSection::right;
        if (section == "nameOfNode" || section == "name_of_node") return PartialChunkSection::name_of_node;
        if (section == "nodeOfName" || section == "node_of_name") return PartialChunkSection::node_of_name;
        throw std::runtime_error("Unknown partial-query chunk section: " + section);
    }

    PartialQueryManifest PartialQueryManifest::load(const std::string& source, const std::string&)
    {
        PartialQueryManifest result;
        result._source = source;
        result._local_path = detail::is_hf_uri(source)
                           ? detail::fetch_chunk_to_cache(source, 0, 0, "manifest").string()
                           : std::filesystem::absolute(source).string();
        const std::string text = read_all(result._local_path);
        result._legacy = detail::parse_manifest_file(result._local_path);
        result._manifest_sha256 = sha256::file(result._local_path);

        std::string schema;
        detail::parse_json_string_field(text, "schemaVersion", schema);
        result._canonical = schema == "zelph-partial-query-manifest-v1";

        if (result._canonical)
        {
            const auto dataset = detail::find_json_object(text, "dataset");
            if (dataset.empty()) throw std::runtime_error("Canonical partial-query manifest is missing dataset metadata");
            detail::parse_json_string_field(dataset, "id", result._dataset_id);
            detail::parse_json_string_field(dataset, "version", result._dataset_version);
            const auto declared_hash = detail::find_json_object(dataset, "manifestHash");
            std::string declared_value;
            std::string declared_algorithm;
            if (!declared_hash.empty())
            {
                detail::parse_json_string_field(declared_hash, "algorithm", declared_algorithm);
                detail::parse_json_string_field(declared_hash, "value", declared_value);
            }
            const auto calculated_contract_hash = sha256::bytes(blank_declared_manifest_hash(text));
            result._manifest_hash_valid = declared_algorithm == "sha256"
                                       && sha256::normalize(declared_value) == calculated_contract_hash;
            if (result._dataset_id.empty() || result._dataset_version.empty())
                throw std::runtime_error("Canonical partial-query manifest requires dataset.id and dataset.version");
            if (!result._manifest_hash_valid)
                throw std::runtime_error("Canonical partial-query manifest contract hash is invalid");

            const auto partitioning = detail::find_json_object(text, "partitioning");
            if (!partitioning.empty()) detail::parse_json_bool_field(partitioning, "globalCoverage", result._global_coverage);

            const auto representations = detail::find_json_object(text, "representationLayers");
            if (representations.empty()) throw std::runtime_error("Canonical partial-query manifest is missing representationLayers");
            const std::pair<const char*, const char*> layer_fields[] = {
                {"directClaim", "directClaim"}, {"statement", "statement"}, {"mainSnak", "mainSnak"},
                {"qualifier", "qualifier"}, {"reference", "reference"}, {"rank", "rank"}, {"names", "names"}};
            for (const auto& [field, name] : layer_fields)
            {
                bool available = false;
                if (detail::parse_json_bool_field(representations, field, available) && available) result._layers.insert(name);
            }

            const auto shards = detail::find_json_array(text, "shards");
            if (shards.empty()) throw std::runtime_error("Canonical partial-query manifest is missing shards");
            for_each_object(shards, [&](std::string_view object)
            {
                PartialChunkDescriptor descriptor;
                std::string section;
                uint64_t index = 0;
                detail::parse_json_string_field(object, "id", descriptor.id);
                if (!detail::parse_json_string_field(object, "section", section)
                    || !detail::parse_json_number_field(object, "chunkIndex", index)
                    || !detail::parse_json_number_field(object, "byteSize", descriptor.byte_size))
                    throw std::runtime_error("Canonical shard requires section, chunkIndex, and byteSize");
                descriptor.section = parse_section(section);
                descriptor.chunk_index = static_cast<uint32_t>(index);
                descriptor.sha256 = digest_value(object);
                descriptor.uri = first_transport_uri(object);
                descriptor.layer = first_layer(object);
                detail::parse_json_number_field(object, "sourceOffset", descriptor.source_offset);
                descriptor.has_source_offset = detail::find_json_key_position(object, "sourceOffset") != std::string_view::npos;
                if (descriptor.id.empty() || descriptor.uri.empty() || descriptor.sha256.empty())
                    throw std::runtime_error("Canonical shard requires id, transport URI, and SHA-256 digest");
                result.add_chunk(std::move(descriptor));
            });

            const auto indexes = detail::find_json_array(text, "routingIndexes");
            for_each_object(indexes, [&](std::string_view object)
            {
                std::string key;
                detail::parse_json_string_field(object, "key", key);
                if (key != "subject" && key != "node" && key != "subjectPredicate") return;
                auto& descriptor = result._node_routing_index;
                detail::parse_json_string_field(object, "id", descriptor.id);
                descriptor.key = key;
                descriptor.uri = first_transport_uri(object);
                descriptor.sha256 = digest_value(object);
                detail::parse_json_string_field(object, "formatVersion", descriptor.format_version);
                detail::parse_json_bool_field(object, "exhaustive", descriptor.exhaustive);
            });
            if (result._node_routing_index.id.empty() || result._node_routing_index.uri.empty()
                || result._node_routing_index.sha256.empty() || !result._node_routing_index.exhaustive)
                throw std::runtime_error("Canonical partial-query manifest requires an exhaustive, hashed node routing index");
        }
        else
        {
            result._dataset_id = "legacy";
            result._dataset_version = result._manifest_sha256;
            result._layers.insert("directClaim");
            result._layers.insert("names");
            result._node_routing_index.id = "legacy-node-route";
            result._node_routing_index.key = "node";
            result._node_routing_index.uri = !result._legacy.node_route_index_local_path.empty()
                                              ? result._legacy.node_route_index_local_path
                                              : result._legacy.node_route_index_path;
            result._node_routing_index.exhaustive = result._legacy.node_route_supported;
        }

        add_legacy_chunks(result, PartialChunkSection::left, result._legacy.left);
        add_legacy_chunks(result, PartialChunkSection::right, result._legacy.right);
        add_legacy_chunks(result, PartialChunkSection::name_of_node, result._legacy.name_of_node);
        add_legacy_chunks(result, PartialChunkSection::node_of_name, result._legacy.node_of_name);
        return result;
    }

    bool PartialQueryManifest::certifiable() const
    {
        if (!_canonical || !_manifest_hash_valid || !_node_routing_index.exhaustive || _chunks.empty()) return false;
        for (const auto& [_, chunk] : _chunks)
            if (chunk.sha256.empty() || chunk.uri.empty()) return false;
        return true;
    }

    bool PartialQueryManifest::layer_available(const std::string& layer) const { return _layers.contains(layer); }

    const PartialChunkDescriptor* PartialQueryManifest::chunk(const PartialChunkSection section, const uint32_t index) const
    {
        const auto it = _chunks.find({section, index});
        return it == _chunks.end() ? nullptr : &it->second;
    }

    std::vector<PartialChunkDescriptor> PartialQueryManifest::chunks(const PartialChunkSection section) const
    {
        std::vector<PartialChunkDescriptor> result;
        for (const auto& [key, value] : _chunks) if (key.first == section) result.push_back(value);
        return result;
    }

    void PartialQueryManifest::add_chunk(PartialChunkDescriptor descriptor)
    {
        _chunks[{descriptor.section, descriptor.chunk_index}] = std::move(descriptor);
    }

    std::string PartialQueryManifest::coverage_summary() const
    {
        std::ostringstream out;
        out << "dataset=" << _dataset_id << "@" << _dataset_version
            << ", canonical=" << (_canonical ? "true" : "false")
            << ", certifiable=" << (certifiable() ? "true" : "false")
            << ", routing_exhaustive=" << (_node_routing_index.exhaustive ? "true" : "false")
            << ", chunks=" << _chunks.size() << ", layers=";
        bool first = true;
        for (const auto& layer : _layers)
        {
            if (!first) out << ',';
            first = false;
            out << layer;
        }
        return out.str();
    }
} // namespace zelph::network
#endif
