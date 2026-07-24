/*
Copyright (c) 2026 acrion innovations GmbH

Capability-aware script import policy used by partial-graph sessions.
*/
#pragma once

#include "platform/platform_utils.hpp"
#include "string/string_utils.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zelph::console::import_policy
{
    enum class ImportCapability { language_extension, graph_transform, rule_program };
    enum class DeclarationSource { none, inline_header, installed_sidecar };

    struct ImportDescriptor
    {
        ImportCapability capability = ImportCapability::rule_program;
        DeclarationSource declaration_source = DeclarationSource::none;
        std::filesystem::path resolved_path;
        std::filesystem::path declaration_path;
        bool trusted_standard_library = false;
        std::vector<std::filesystem::path> companions;

        bool partial_language_extension_allowed() const
        {
            return capability == ImportCapability::language_extension
                && declaration_source == DeclarationSource::installed_sidecar
                && trusted_standard_library;
        }
    };

    inline const char* capability_name(const ImportCapability capability)
    {
        switch (capability)
        {
        case ImportCapability::language_extension: return "language-extension";
        case ImportCapability::graph_transform: return "graph-transform";
        case ImportCapability::rule_program: return "rule-program";
        }
        return "rule-program";
    }

    inline std::filesystem::path canonical_if_possible(const std::filesystem::path& value)
    {
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(value, error);
        return error ? std::filesystem::absolute(value) : canonical;
    }

    inline bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        const auto canonical_path = canonical_if_possible(path);
        const auto canonical_root = canonical_if_possible(root);
        auto path_it = canonical_path.begin();
        auto root_it = canonical_root.begin();
        for (; root_it != canonical_root.end(); ++root_it, ++path_it)
            if (path_it == canonical_path.end() || *path_it != *root_it) return false;
        return true;
    }

    inline std::filesystem::path resolve_script_reference(const std::string& raw)
    {
        namespace fs = std::filesystem;
        const std::string extension = fs::path(raw).extension().string();
        if (!extension.empty() && extension != ".zph" && extension != ".janet")
            throw std::runtime_error("Script '" + raw + "': only '.zph' and '.janet' scripts can be imported");

        std::vector<fs::path> variants{fs::path(raw)};
        if (extension.empty()) variants.emplace_back(raw + ".zph");
        for (const auto& variant : variants)
        {
            std::error_code error;
            if (fs::is_regular_file(variant, error)) return canonical_if_possible(variant);
        }
        if (!fs::path(raw).is_absolute())
        {
            for (const auto& base : platform::get_standard_library_paths())
            {
                for (const auto& variant : variants)
                {
                    std::error_code error;
                    const fs::path candidate = base / variant;
                    if (fs::is_regular_file(candidate, error)) return canonical_if_possible(candidate);
                }
            }
        }
        throw std::runtime_error("Script '" + raw + "' not found while evaluating its import capability");
    }

    inline ImportCapability parse_capability(const std::string& value, const std::filesystem::path& source)
    {
        if (value == "language-extension") return ImportCapability::language_extension;
        if (value == "graph-transform") return ImportCapability::graph_transform;
        if (value == "rule-program") return ImportCapability::rule_program;
        throw std::runtime_error("Unknown zelph import capability '" + value + "' in " + source.string());
    }

    inline bool trusted_standard_path(const std::filesystem::path& path)
    {
        for (const auto& root : platform::get_standard_library_paths())
            if (path_is_within(path, root)) return true;
        return false;
    }

    inline bool read_sidecar(const std::filesystem::path& path, ImportDescriptor& result)
    {
        std::ifstream input(path);
        if (!input) return false;
        std::string line;
        bool has_version = false;
        bool has_capability = false;
        std::vector<std::string> companion_names;
        while (std::getline(input, line))
        {
            string::trim_in_place(line);
            if (line.empty() || line.starts_with('#')) continue;
            const auto equals = line.find('=');
            if (equals == std::string::npos)
                throw std::runtime_error("Malformed import capability sidecar " + path.string());
            auto key = line.substr(0, equals);
            auto value = line.substr(equals + 1);
            string::trim_in_place(key);
            string::trim_in_place(value);
            if (key == "version")
            {
                if (value != "1") throw std::runtime_error("Unsupported import capability sidecar version in " + path.string());
                has_version = true;
            }
            else if (key == "capability")
            {
                result.capability = parse_capability(value, path);
                has_capability = true;
            }
            else if (key == "companion")
            {
                if (value.empty()) throw std::runtime_error("Empty companion declaration in " + path.string());
                companion_names.push_back(value);
            }
        }
        if (!has_version || !has_capability)
            throw std::runtime_error("Import capability sidecar requires version=1 and capability=...: " + path.string());
        result.declaration_source = DeclarationSource::installed_sidecar;
        result.declaration_path = path;
        for (const auto& name : companion_names)
        {
            const auto companion = resolve_script_reference(name);
            if (companion.extension() != ".zph" || !trusted_standard_path(companion))
                throw std::runtime_error("Language-extension companion is not an installed standard-library .zph script: " + companion.string());
            result.companions.push_back(companion);
        }
        return true;
    }

    inline ImportDescriptor inspect(const std::string& raw)
    {
        ImportDescriptor result;
        result.resolved_path = resolve_script_reference(raw);
        result.trusted_standard_library = trusted_standard_path(result.resolved_path);
        if (result.resolved_path.extension() != ".zph") return result;

        const auto sidecar = std::filesystem::path(result.resolved_path.string() + ".capability");
        if (read_sidecar(sidecar, result)) return result;

        std::ifstream input(result.resolved_path);
        if (!input) throw std::runtime_error("Cannot inspect script '" + result.resolved_path.string() + "'");
        constexpr const char* prefix = "# zelph-import-capability:";
        std::string line;
        size_t inspected = 0;
        while (inspected++ < 64 && std::getline(input, line))
        {
            string::trim_in_place(line);
            if (line.empty()) continue;
            if (!line.starts_with('#')) break;
            if (!line.starts_with(prefix)) continue;
            auto value = line.substr(std::char_traits<char>::length(prefix));
            string::trim_in_place(value);
            result.capability = parse_capability(value, result.resolved_path);
            result.declaration_source = DeclarationSource::inline_header;
            result.declaration_path = result.resolved_path;
            return result;
        }
        return result;
    }
} // namespace zelph::console::import_policy
