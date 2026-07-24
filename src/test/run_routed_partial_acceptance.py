#!/usr/bin/env python3
"""Hermetic full-graph versus routed-partial SPARQL acceptance test."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any

MAX_NODE = 18446744073709551615
QUERY_NAMES = [
    "Q1", "Q2", "Q5", "Q30", "Q50", "Q100", "Q200", "Q408", "Q500", "QD",
    "P17", "P19", "P31", "P279", "P570",
]


def run_zelph(executable: Path, script: str, cwd: Path, *, expect_success: bool = True) -> str:
    env = dict(os.environ)
    env.setdefault("ZELPH_HF_CACHE_DIR", str(cwd / "cache"))
    completed = subprocess.run(
        [str(executable)],
        input=script,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=cwd,
        env=env,
        timeout=120,
    )
    if expect_success and completed.returncode != 0:
        raise AssertionError(f"zelph failed ({completed.returncode}):\n{completed.stdout}")
    if not expect_success and completed.returncode == 0 and "Error" not in completed.stdout:
        raise AssertionError(f"zelph unexpectedly accepted query:\n{completed.stdout}")
    return completed.stdout


def write_legacy_manifest(index: dict[str, Any], root: Path, bin_path: Path, route_path: Path) -> Path:
    sections: dict[str, Any] = {}
    for section_name in ("left", "right", "nameOfNode", "nodeOfName"):
        chunks = []
        for item in index[section_name]:
            chunk = {
                "chunkIndex": int(item["chunkIndex"]),
                "sourceOffset": int(item["offset"]),
                "length": int(item["length"]),
            }
            if "which" in item:
                chunk["which"] = item["which"]
            if "lang" in item:
                chunk["lang"] = item["lang"]
            chunks.append(chunk)
        sections[section_name] = {"chunks": chunks}

    document = {
        "layout": "zelph-hf-layout/v3",
        "source": {
            "binPath": str(bin_path),
            "headerLengthBytes": int(index["header"]["length"]),
        },
        "sections": sections,
        "capabilities": {"nodeRouteIndex": True, "features": ["node-route"]},
        "hfObjects": {"nodeRouteIndex": {"localPath": str(route_path)}},
    }
    path = root / "legacy-manifest.json"
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def write_route_sidecar(index: dict[str, Any], path: Path) -> None:
    routing: dict[str, list[dict[str, Any]]] = {}
    for section_name in ("left", "right", "nameOfNode"):
        routing[section_name] = [
            {
                "chunkIndex": int(item["chunkIndex"]),
                "range": {"min": 0, "max": MAX_NODE},
            }
            for item in index[section_name]
        ]
    routing["nodeOfName"] = [
        {
            "chunkIndex": int(item["chunkIndex"]),
            "lang": item.get("lang", ""),
            "names": QUERY_NAMES,
        }
        for item in index["nodeOfName"]
    ]
    path.write_text(json.dumps({"routing": routing}, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def query_script(load_command: str, query: str) -> str:
    return f"""{load_command}
.import sparql
sparql
{query}

.quit
"""


def q_values(output: str) -> list[str]:
    values = re.findall(r"\|\s*(Q[A-Za-z0-9]+)\s*\|", output)
    return sorted(set(values))


def numeric_values(output: str) -> list[int]:
    return [int(value) for value in re.findall(r"\|\s*([0-9]+)\s*\|", output)]


def assert_equivalent(executable: Path, cwd: Path, full_load: str, routed_load: str,
                      query: str, extractor) -> str:
    full_output = run_zelph(executable, query_script(full_load, query), cwd)
    routed_output = run_zelph(executable, query_script(routed_load, query), cwd)
    full_values = extractor(full_output)
    routed_values = extractor(routed_output)
    if full_values != routed_values:
        raise AssertionError(
            f"full/routed mismatch for query:\n{query}\nfull={full_values}\nrouted={routed_values}\n"
            f"FULL OUTPUT:\n{full_output}\nROUTED OUTPUT:\n{routed_output}"
        )
    if '"completeness":"complete"' not in routed_output:
        raise AssertionError(f"routed query did not certify completeness:\n{routed_output}")
    if '"type":"shard_loaded"' not in routed_output:
        raise AssertionError(f"routed query did not load a shard on demand:\n{routed_output}")
    if "Routed SPARQL join strategy loaded" not in routed_output:
        raise AssertionError(f"partial SPARQL companion was not loaded:\n{routed_output}")
    return routed_output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--zelph", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()

    executable = args.zelph.resolve()
    source_root = args.source_root.resolve()
    with tempfile.TemporaryDirectory(prefix="zelph-routed-acceptance-") as temp:
        root = Path(temp)
        bin_path = root / "tiny.bin"
        index_path = root / "tiny.index.json"

        build_script = f""".lang wikidata
Q1 P31 Q5
Q2 P31 Q5
Q1 P19 Q100
Q2 P19 Q200
Q100 P17 Q408
Q200 P17 Q30
Q5 P279 Q50
Q50 P279 Q500
Q2 P570 QD
.save {bin_path}
.index-file {bin_path} {index_path}
.quit
"""
        run_zelph(executable, build_script, root)
        index = json.loads(index_path.read_text(encoding="utf-8"))

        route_path = root / "node-route.json"
        write_route_sidecar(index, route_path)
        legacy_manifest = write_legacy_manifest(index, root, bin_path, route_path)

        artifact = root / "artifact"
        emitter = source_root / "tools" / "emit_partial_query_manifest.py"
        subprocess.run(
            [
                os.environ.get("PYTHON", "python3"), str(emitter),
                str(legacy_manifest), str(artifact),
                "--dataset-id", "zelph-routed-test",
                "--dataset-version", "1",
                "--route-index", str(route_path),
                "--source-bin", str(bin_path),
            ],
            check=True,
            cwd=root,
            timeout=120,
        )

        manifest = artifact / "manifest.json"
        full_load = f".load {bin_path}"
        routed_load = f".load-partial {manifest} meta-only"

        join_output = assert_equivalent(
            executable, root, full_load, routed_load,
            "SELECT ?country WHERE {\n  wd:Q1 wdt:P19 ?place .\n  ?place wdt:P17 ?country .\n}",
            q_values,
        )
        if q_values(join_output) != ["Q408"]:
            raise AssertionError(f"unexpected joined result: {q_values(join_output)}")

        path_output = assert_equivalent(
            executable, root, full_load, routed_load,
            "SELECT ?ancestor WHERE { wd:Q5 wdt:P279* ?ancestor . }",
            q_values,
        )
        if q_values(path_output) != ["Q5", "Q50", "Q500"]:
            raise AssertionError(f"unexpected path closure: {q_values(path_output)}")

        count_output = assert_equivalent(
            executable, root, full_load, routed_load,
            "SELECT (COUNT(?x) AS ?n) WHERE { ?x wdt:P31 wd:Q5 . }",
            numeric_values,
        )
        if 2 not in numeric_values(count_output):
            raise AssertionError(f"unexpected count result:\n{count_output}")

        ordered_output = assert_equivalent(
            executable, root, full_load, routed_load,
            "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . } ORDER BY ?x LIMIT 1",
            q_values,
        )
        if len(q_values(ordered_output)) != 1:
            raise AssertionError(f"unexpected ordered window:\n{ordered_output}")

        minus_output = assert_equivalent(
            executable, root, full_load, routed_load,
            "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . MINUS { ?x wdt:P570 ?d . } }",
            q_values,
        )
        if q_values(minus_output) != ["Q1"]:
            raise AssertionError(f"unexpected MINUS result: {q_values(minus_output)}")

        missing_layer_query = "SELECT ?s WHERE { wd:Q1 p:P39 ?s . ?s pq:P580 ?date . }"
        refused = run_zelph(
            executable,
            query_script(routed_load, missing_layer_query),
            root,
            expect_success=False,
        )
        if "representation layers absent" not in refused:
            raise AssertionError(f"missing qualifier layer was not reported explicitly:\n{refused}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
