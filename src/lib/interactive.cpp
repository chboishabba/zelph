/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

#include "interactive.hpp"

#include "command_executor.hpp"
#include "import_policy.hpp"
#include "network/reasoning.hpp"
#include "partial_sparql.hpp"
#include "repl_state.hpp"
#include "script_engine.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace zelph;

namespace
{
    std::atomic<uint64_t> partial_scope_serial{0};

    std::string partial_scope_name(const char* kind)
    {
        return "__zelph_partial_" + std::string(kind) + "_" + std::to_string(++partial_scope_serial);
    }

    void restore_cluster(network::Reasoning* graph, const std::string& previous)
    {
        if (previous.empty() || previous == "default")
            graph->deactivate_cluster();
        else
            graph->set_active_cluster(previous);
    }

    struct GraphShape
    {
        network::Node nodes = 0;
        size_t rules = 0;
        std::vector<std::tuple<std::string, size_t, size_t>> language_names;

        friend bool operator==(const GraphShape&, const GraphShape&) = default;
    };

    GraphShape graph_shape(const network::Reasoning& graph)
    {
        GraphShape shape;
        shape.nodes = graph.count();
        shape.rules = graph.rule_count();
        auto languages = graph.get_languages();
        std::sort(languages.begin(), languages.end());
        for (const auto& language : languages)
            shape.language_names.emplace_back(
                language,
                graph.get_name_of_node_size(language),
                graph.get_node_of_name_size(language));
        return shape;
    }

    bool keyword_text_complete(const std::string& text)
    {
        const auto upper = console::partial_sparql::lexical_upper(text);
        if (upper.find("SELECT") == std::string::npos || text.find('{') == std::string::npos) return false;
        int depth = 0;
        for (const char c : text)
        {
            if (c == '{') ++depth;
            if (c == '}') --depth;
        }
        return depth == 0;
    }
}

class console::Interactive::Impl
{
public:
    explicit Impl(Interactive* enclosing, io::OutputHandler output)
        : _repl_state(std::make_shared<ReplState>())
        , _interactive(enclosing)
    {
        init(output);
    }

    void init(io::OutputHandler output)
    {
        _n             = std::make_unique<network::Reasoning>(output);
        _script_engine = std::make_unique<ScriptEngine>(_n.get());

        _n->set_lang("zelph");

        _n->register_core_node(_n->core.RelationTypeCategory, "->");
        _n->register_core_node(_n->core.Causes, "=>");
        _n->register_core_node(_n->core.IsA, "~");
        _n->register_core_node(_n->core.Unequal, "!=");
        _n->register_core_node(_n->core.Contradiction, "!");
        _n->register_core_node(_n->core.Cons, "cons");
        _n->register_core_node(_n->core.Nil, "nil");
        _n->register_core_node(_n->core.PartOf, "in");
        _n->register_core_node(_n->core.Conjunction, "conjunction");
        _n->register_core_node(_n->core.Negation, "negation");

        _script_engine->initialize();

        _command_executor = std::make_unique<CommandExecutor>(
            _n.get(),
            _script_engine.get(),
            _repl_state,
            [this](const std::string& line)
            { _interactive->process(line); });

        _script_engine->set_import_handler(
            [this](const std::string& path, const std::vector<std::string>& args)
            { import_with_partial_policy(path, args); });

        _script_engine->set_command_handler(
            [this](const std::vector<std::string>& cmd)
            { _command_executor->execute(cmd); });
    }

    void reset_reasoning()
    {
        _command_executor.reset();
        _script_engine.reset();
        auto output = _n->get_output_handler();
        _n.reset();

#ifndef __EMSCRIPTEN__
        _repl_state->partial_load_mode = false;
        _repl_state->partial_load_source.clear();
        _repl_state->partial_language_extension_import = false;
#endif
        _repl_state->janet_buffer.clear();
        _repl_state->zelph_buffer.clear();
        _repl_state->accumulating_inline_janet = false;
        _repl_state->accumulating_zelph        = false;
        _repl_state->script_mode               = ScriptMode::Zelph;
        _repl_state->reset_requested           = false;
        _repl_state->accumulating_keyword      = false;
        _repl_state->active_keyword.clear();
        _repl_state->keyword_buffer.clear();
        _repl_state->keyword_prev_blank = false;

        zelph::string::reset_last_node();

        init(output);
        _n->out("Cleared network and re-initialized core nodes.");
    }

    void import_with_partial_policy(const std::string& path, const std::vector<std::string>& args)
    {
#ifndef __EMSCRIPTEN__
        if (_repl_state->partial_load_mode)
        {
            const auto descriptor = import_policy::inspect(path);
            if (!descriptor.partial_language_extension_allowed())
            {
                throw std::runtime_error(
                    "Import blocked in partial load mode: '" + descriptor.resolved_path.string()
                    + "' is classified as " + import_policy::capability_name(descriptor.capability)
                    + ". Only installed standard-library language extensions with a trusted capability sidecar are permitted.");
            }

            const auto before = partial_sparql::fingerprint(*_n);
            const auto previous_cluster = _n->active_cluster_name();
            const auto audit_cluster = partial_scope_name("import");
            const bool previous_import_state = _repl_state->partial_language_extension_import;
            _n->set_active_cluster(audit_cluster);
            _repl_state->partial_language_extension_import = true;

            try
            {
                _command_executor->import_file(descriptor.resolved_path.string(), args);
            }
            catch (...)
            {
                _repl_state->partial_language_extension_import = previous_import_state;
                _n->deactivate_cluster();
                _n->drop_cluster(audit_cluster);
                restore_cluster(_n.get(), previous_cluster);
                const auto restored = partial_sparql::fingerprint(*_n);
                if (!(restored == before))
                {
                    throw std::runtime_error(
                        "Partial language-extension import failed and graph rollback was incomplete. Before: "
                        + partial_sparql::describe(before) + "; restored: " + partial_sparql::describe(restored));
                }
                throw;
            }

            _repl_state->partial_language_extension_import = previous_import_state;
            const auto after_import = partial_sparql::fingerprint(*_n);
            _n->deactivate_cluster();
            const auto rolled_back_nodes = _n->drop_cluster(audit_cluster);
            restore_cluster(_n.get(), previous_cluster);
            const auto restored = partial_sparql::fingerprint(*_n);

            if (!(after_import == before))
            {
                if (!(restored == before))
                {
                    throw std::runtime_error(
                        "Declared language extension mutated the partial graph and rollback was incomplete. Before: "
                        + partial_sparql::describe(before) + "; restored: " + partial_sparql::describe(restored));
                }
                throw std::runtime_error(
                    "Declared language extension violated the graph-preservation invariant; "
                    + std::to_string(rolled_back_nodes) + " cluster node(s) were rolled back.");
            }
            if (!(restored == before))
            {
                throw std::runtime_error("Language-extension import changed graph state outside its audit cluster");
            }

            _n->diagnostic("Imported partial-safe language extension: " + descriptor.resolved_path.string(), true);
            return;
        }
#endif
        _command_executor->import_file(path, args);
    }

    bool invoke_keyword_with_partial_policy(const std::string& keyword, const std::string& text, const bool force)
    {
#ifndef __EMSCRIPTEN__
        if (_repl_state->partial_load_mode)
        {
            if (keyword != "sparql")
                throw std::runtime_error("Registered keyword '" + keyword + "' is not certified for partial-graph execution");

            if (!force && !keyword_text_complete(text))
                return _script_engine->invoke_keyword(keyword, text, force);

            const auto assessment = partial_sparql::classify(text);
            if (!assessment.allowed_in_resident_slice)
            {
                throw std::runtime_error(
                    "SPARQL partial execution refused (" + std::string(partial_sparql::query_class_name(assessment.query_class))
                    + "): " + assessment.reason);
            }

            _n->out("SPARQL execution metadata:", true);
            _n->out("  dataset_mode: partial", true);
            _n->out("  evaluation_scope: resident_graph", true);
            _n->out("  query_class: " + std::string(partial_sparql::query_class_name(assessment.query_class)), true);
            _n->out("  row_contract: " + std::string(partial_sparql::result_contract_name(assessment.result_contract)), true);
            _n->out("  global_completeness: not_established", true);

            // Keep the query hot path proportional to query work, not the
            // entire resident graph. New query nodes are isolated by the
            // temporary cluster; this shape check detects surviving changes.
            const auto before_shape = graph_shape(*_n);
            const auto previous_cluster = _n->active_cluster_name();
            const auto query_cluster = partial_scope_name("query");
            _n->set_active_cluster(query_cluster);

            bool dispatched = false;
            try
            {
                dispatched = _script_engine->invoke_keyword(keyword, text, force);
            }
            catch (...)
            {
                _n->deactivate_cluster();
                _n->drop_cluster(query_cluster);
                restore_cluster(_n.get(), previous_cluster);
                const auto restored_shape = graph_shape(*_n);
                if (!(restored_shape == before_shape))
                    throw std::runtime_error("Partial SPARQL failed and its ephemeral graph rollback was incomplete");
                throw;
            }

            _n->deactivate_cluster();
            const auto ephemeral_nodes = _n->drop_cluster(query_cluster);
            restore_cluster(_n.get(), previous_cluster);
            const auto restored_shape = graph_shape(*_n);
            if (!(restored_shape == before_shape))
                throw std::runtime_error("Partial SPARQL violated the read-only graph-shape invariant");

            if (dispatched)
            {
                _n->out("SPARQL completion metadata:", true);
                _n->out("  result_set_status: incomplete", true);
                _n->out("  completeness_reason: resident graph only; routing fixed point not attempted", true);
                _n->out("  ephemeral_graph_nodes_rolled_back: " + std::to_string(ephemeral_nodes), true);
            }
            return dispatched;
        }
#endif
        return _script_engine->invoke_keyword(keyword, text, force);
    }

    void process_command(const std::vector<std::string>& cmd)
    {
#ifndef __EMSCRIPTEN__
        if (_repl_state->partial_load_mode && !cmd.empty())
        {
            if (cmd[0] == ".import")
            {
                if (cmd.size() < 2) throw std::runtime_error("Command .import: Missing script path");
                import_with_partial_policy(cmd[1], std::vector<std::string>(cmd.begin() + 2, cmd.end()));
                return;
            }
            if (cmd[0] == ".auto-run")
                throw std::runtime_error("Command .auto-run is blocked while a partial graph is loaded");
        }
#endif
        _command_executor->execute(cmd);

        if (_repl_state->reset_requested)
        {
            _repl_state->reset_requested = false;
            reset_reasoning();
        }
    }

    std::unique_ptr<network::Reasoning> _n;
    std::unique_ptr<ScriptEngine>       _script_engine;
    std::unique_ptr<CommandExecutor>    _command_executor;
    std::shared_ptr<ReplState>          _repl_state;

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

private:
    const Interactive* _interactive;
};

console::Interactive::Interactive(io::OutputHandler output)
    : _pImpl(new Impl(this, std::move(output)))
{
}

console::Interactive::~Interactive()
{
    delete _pImpl;
}

void console::Interactive::process_file(const std::string& file, const std::vector<std::string>& args) const
{
    _pImpl->import_with_partial_policy(file, args);
}

std::string console::Interactive::get_version()
{
    return network::Zelph::get_version();
}

bool console::Interactive::is_auto_run_active() const
{
    return _pImpl->_repl_state->auto_run;
}

bool console::Interactive::is_accumulating() const
{
    const auto& s = _pImpl->_repl_state;
    return s->accumulating_zelph
        || s->accumulating_inline_janet
        || s->accumulating_keyword
        || s->script_mode == ScriptMode::Janet;
}

void console::Interactive::process(std::string line) const
{
    try
    {
        auto& state = _pImpl->_repl_state;

        if (state->accumulating_keyword)
        {
            if (line.find_first_not_of(" \t\r") == std::string::npos)
            {
                const bool force = state->keyword_prev_blank;
                _pImpl->_n->profiler_reset_epoch();

                bool dispatched = false;
                try
                {
                    dispatched = _pImpl->invoke_keyword_with_partial_policy(
                        state->active_keyword, state->keyword_buffer, force);
                }
                catch (...)
                {
                    state->accumulating_keyword = false;
                    state->active_keyword.clear();
                    state->keyword_buffer.clear();
                    state->keyword_prev_blank = false;
                    throw;
                }

                if (dispatched)
                {
                    state->accumulating_keyword = false;
                    state->active_keyword.clear();
                    state->keyword_buffer.clear();
                    state->keyword_prev_blank = false;

                    if (state->auto_run)
                        _pImpl->_n->run(true, false, false, true);
                }
                else
                {
                    state->keyword_buffer += "\n";
                    state->keyword_prev_blank = true;
                }
            }
            else
            {
                state->keyword_buffer += line + "\n";
                state->keyword_prev_blank = false;
            }
            return;
        }

        if (!line.empty() && line[0] == '#') return;

        size_t first_char_pos = line.find_first_not_of(" \t");

        if (first_char_pos != std::string::npos && line[first_char_pos] == '.')
        {
            std::vector<std::string> parts = zelph::string::tokenize_quoted(line);
            if (!parts.empty() && !parts[0].empty() && parts[0][0] == '.')
            {
                _pImpl->_n->profiler_reset_epoch();
                _pImpl->process_command(parts);
                return;
            }
        }

        if (first_char_pos == std::string::npos) return;

        if (state->accumulating_inline_janet)
        {
#ifndef __EMSCRIPTEN__
            if (state->partial_load_mode && !state->partial_language_extension_import)
                throw std::runtime_error("Raw Janet input is blocked while a partial graph is loaded");
#endif
            state->janet_buffer += line + "\n";
            if (zelph::ScriptEngine::is_expression_complete(state->janet_buffer))
            {
                _pImpl->_script_engine->process_janet(state->janet_buffer, false);
                state->janet_buffer.clear();
                state->accumulating_inline_janet = false;
                if (state->auto_run)
                    _pImpl->_n->run(true, false, false, true);
            }
            return;
        }

        std::string trimmed_utf8 = zelph::string::trim(line);

        if (trimmed_utf8 == "%")
        {
#ifndef __EMSCRIPTEN__
            if (state->partial_load_mode && !state->partial_language_extension_import)
                throw std::runtime_error("Raw Janet input is blocked while a partial graph is loaded");
#endif
            if (state->script_mode == ScriptMode::Janet)
            {
                if (!state->janet_buffer.empty())
                {
                    _pImpl->_n->profiler_reset_epoch();
                    const std::string code = state->janet_buffer;
                    state->janet_buffer.clear();
                    state->script_mode = ScriptMode::Zelph;
                    _pImpl->_script_engine->process_janet(code, false);
                    if (state->auto_run)
                        _pImpl->_n->run(true, false, false, true);
                }
                else
                {
                    state->script_mode = ScriptMode::Zelph;
                }
            }
            else
            {
                state->script_mode = ScriptMode::Janet;
            }
            return;
        }

        if (trimmed_utf8[0] == '%')
        {
#ifndef __EMSCRIPTEN__
            if (state->partial_load_mode && !state->partial_language_extension_import)
                throw std::runtime_error("Raw Janet input is blocked while a partial graph is loaded");
#endif
            std::string janet_code = trimmed_utf8.substr(1);
            janet_code             = zelph::string::trim_left(janet_code);
            if (janet_code.empty()) return;

            if (zelph::ScriptEngine::is_expression_complete(janet_code))
            {
                _pImpl->_n->profiler_reset_epoch();
                _pImpl->_script_engine->process_janet(janet_code, false);
                if (state->auto_run)
                    _pImpl->_n->run(true, false, false, true);
            }
            else
            {
                state->janet_buffer              = janet_code + "\n";
                state->accumulating_inline_janet = true;
            }
            return;
        }

        if (state->script_mode == ScriptMode::Janet)
        {
#ifndef __EMSCRIPTEN__
            if (state->partial_load_mode && !state->partial_language_extension_import)
                throw std::runtime_error("Raw Janet input is blocked while a partial graph is loaded");
#endif
            state->janet_buffer += line + "\n";
            return;
        }

        if (!state->accumulating_zelph)
        {
            size_t      end_of_token = trimmed_utf8.find_first_of(" \t");
            std::string first_token  = trimmed_utf8.substr(0, end_of_token);
            if (_pImpl->_script_engine->has_keyword(first_token))
            {
#ifndef __EMSCRIPTEN__
                if (state->partial_load_mode && first_token != "sparql")
                    throw std::runtime_error("Registered keyword '" + first_token + "' is not certified for partial-graph execution");
#endif
                state->active_keyword       = first_token;
                state->accumulating_keyword = true;
                if (end_of_token != std::string::npos)
                {
                    std::string rest = zelph::string::trim_left(trimmed_utf8.substr(end_of_token));
                    if (!rest.empty()) state->keyword_buffer = rest + "\n";
                }
                return;
            }
        }

#ifndef __EMSCRIPTEN__
        if (state->partial_load_mode && !state->partial_language_extension_import)
            throw std::runtime_error("Facts, rules, and ordinary Zelph statements are blocked while a partial graph is loaded");
#endif

        if (state->accumulating_zelph)
            state->zelph_buffer += "\n" + line;
        else
            state->zelph_buffer = line;

        if (!zelph::ScriptEngine::is_zelph_complete(state->zelph_buffer))
        {
            state->accumulating_zelph = true;
            return;
        }

        std::string complete_stmt = state->zelph_buffer;
        state->zelph_buffer.clear();
        state->accumulating_zelph = false;

        std::string transformed = _pImpl->_script_engine->parse_zelph_to_janet(complete_stmt);
        if (!transformed.empty())
        {
            _pImpl->_n->profiler_reset_epoch();
            _pImpl->_script_engine->process_janet(transformed, true);
        }
        else
        {
            size_t u_first = complete_stmt.find_first_not_of(" \t\n");
            if (u_first != std::string::npos)
                throw std::runtime_error("Syntax error: Could not parse statement.");
        }

        if (state->auto_run)
            _pImpl->_n->run(true, false, false, true);
    }
    catch (std::exception& ex)
    {
        throw std::runtime_error("Error in line \"" + line + "\": " + ex.what());
    }
}

void console::Interactive::import_file(const std::string& file) const
{
    _pImpl->import_with_partial_policy(file, {});
}

void console::Interactive::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition) const
{
    _pImpl->_n->run(print_deductions, generate_markdown, suppress_repetition);
}

std::string console::Interactive::get_lang() const
{
    return _pImpl->_n->get_lang();
}

void console::Interactive::set_output_handler(io::OutputHandler output) const
{
    _pImpl->_n->set_output_handler(std::move(output));
}

void console::Interactive::out(const std::string& text, bool newline) const
{
    _pImpl->_n->emit(io::OutputChannel::Out, text, newline);
}

void console::Interactive::err(const std::string& text, bool newline) const
{
    _pImpl->_n->emit(io::OutputChannel::Error, text, newline);
}

void console::Interactive::log(const std::string& text, bool newline) const
{
    _pImpl->_n->emit(io::OutputChannel::Diagnostic, text, newline);
}

void console::Interactive::prompt(const std::string& text, bool newline) const
{
    _pImpl->_n->emit(io::OutputChannel::Prompt, text, newline);
}

#ifdef PROVIDE_C_INTERFACE
console::Interactive interactive;

extern "C" void zelph_process_c(const char* line, size_t len)
{
    if (len > 0)
    {
        std::string l(line, 0, len);
        interactive.process(l);
    }
}

extern "C" void zelph_run()
{
    interactive.run(true, false, false);
}
#endif
