#!/usr/bin/env python3
"""Build a self-contained, content-verified routed-query artifact.

The input is an existing Zelph v2/v3 layout manifest plus its source/header,
chunk objects, and node-route sidecar. The output keeps the legacy `source` and
`sections` fields so current loaders can read it, and adds the canonical v1
contract consumed by routed partial queries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path
from typing import Any, Iterable

SECTIONS = ("left", "right", "nameOfNode", "nodeOfName")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve_local(base: Path, value: str | None) -> Path:
    if not value:
        raise ValueError("missing local artifact path")
    if value.startswith(("hf://", "http://", "https://")):
        raise ValueError(f"artifact producer requires local inputs, got remote URI {value!r}")
    if value.startswith("file://"):
        value = value[7:]
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = base / candidate
    candidate = candidate.resolve()
    if not candidate.is_file():
        raise FileNotFoundError(candidate)
    return candidate


def copy_range(source: Path, output: Path, offset: int, length: int) -> None:
    if length <= 0:
        raise ValueError(f"range length must be positive for {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as src, output.open("wb") as dst:
        src.seek(offset)
        remaining = length
        while remaining:
            block = src.read(min(1024 * 1024, remaining))
            if not block:
                raise EOFError(f"truncated range {source}:{offset}+{length}")
            dst.write(block)
            remaining -= len(block)


def copy_deterministic(source: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as src, output.open("wb") as dst:
        shutil.copyfileobj(src, dst, length=1024 * 1024)


def transport_uri(relative: str, base_uri: str | None) -> str:
    if not base_uri:
        return relative
    return base_uri.rstrip("/") + "/" + relative


def locate_route_index(manifest: dict[str, Any], base: Path, explicit: str | None) -> Path:
    if explicit:
        return resolve_local(base, explicit)
    hf_objects = manifest.get("hfObjects") or {}
    route = hf_objects.get("nodeRouteIndex") or {}
    for key in ("localPath", "path"):
        value = route.get(key)
        if value and not str(value).startswith(("hf://", "http://", "https://")):
            return resolve_local(base, str(value))
    raise ValueError("node-route sidecar not found; pass --route-index")


def parse_layers(values: Iterable[str]) -> dict[str, bool]:
    enabled = {value for value in values}
    unknown = enabled.difference({"directClaim", "statement", "mainSnak", "qualifier", "reference", "rank", "names"})
    if unknown:
        raise ValueError(f"unknown representation layer(s): {', '.join(sorted(unknown))}")
    return {name: name in enabled for name in ("directClaim", "statement", "mainSnak", "qualifier", "reference", "rank", "names")}


def serialise_contract(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path, help="existing v2/v3 manifest")
    parser.add_argument("output", type=Path, help="output artifact directory")
    parser.add_argument("--dataset-id", required=True)
    parser.add_argument("--dataset-version", required=True)
    parser.add_argument("--route-index", help="local node-route sidecar override")
    parser.add_argument("--source-bin", help="local source .bin override")
    parser.add_argument("--base-uri", help="published URI prefix, e.g. hf://datasets/owner/repo/artifact")
    parser.add_argument("--layer", action="append", default=["directClaim", "names"], help="representation layer (repeatable)")
    parser.add_argument("--global-coverage", action="store_true")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    input_manifest = args.manifest.resolve()
    source_base = input_manifest.parent
    with input_manifest.open("r", encoding="utf-8") as handle:
        legacy: dict[str, Any] = json.load(handle)

    output = args.output.resolve()
    if output.exists():
        if not args.force:
            raise FileExistsError(f"output exists: {output}; pass --force")
        shutil.rmtree(output)
    (output / "shards").mkdir(parents=True)
    (output / "routing").mkdir(parents=True)

    source = legacy.get("source") or {}
    source_bin_value = args.source_bin or source.get("binPath") or source.get("path")
    source_bin = resolve_local(source_base, source_bin_value)
    header_length = int(source.get("headerLengthBytes") or source.get("headerLength") or source.get("header_length_bytes") or 0)
    if header_length <= 0:
        raise ValueError("source.headerLengthBytes is required to emit a standalone header object")

    header_relative = "header.capnp"
    header_path = output / header_relative
    copy_range(source_bin, header_path, 0, header_length)

    canonical_sections: dict[str, Any] = {}
    flat_shards: list[dict[str, Any]] = []
    legacy_sections = legacy.get("sections")
    if not isinstance(legacy_sections, dict):
        raise ValueError("input manifest is missing sections")

    for section_name in SECTIONS:
        section = legacy_sections.get(section_name)
        if not isinstance(section, dict) or not isinstance(section.get("chunks"), list):
            raise ValueError(f"input manifest is missing sections.{section_name}.chunks")
        canonical_chunks: list[dict[str, Any]] = []
        for chunk in sorted(section["chunks"], key=lambda item: int(item["chunkIndex"])):
            index = int(chunk["chunkIndex"])
            length = int(chunk["length"])
            relative = f"shards/{section_name}-{index:06d}.capnp"
            destination = output / relative
            object_path = chunk.get("objectPath") or chunk.get("futureObjectPath")
            if object_path:
                candidate = resolve_local(source_base, str(object_path))
                copy_deterministic(candidate, destination)
                if destination.stat().st_size != length:
                    raise ValueError(f"declared length mismatch for {section_name}/{index}")
            else:
                offset = chunk.get("sourceOffset", chunk.get("offset"))
                if offset is None:
                    raise ValueError(f"chunk {section_name}/{index} has neither objectPath nor sourceOffset")
                copy_range(source_bin, destination, int(offset), length)

            digest = sha256_file(destination)
            entry: dict[str, Any] = {
                "chunkIndex": index,
                "length": destination.stat().st_size,
                "objectPath": transport_uri(relative, args.base_uri),
            }
            if chunk.get("which") is not None:
                entry["which"] = chunk["which"]
            if chunk.get("lang") is not None:
                entry["lang"] = chunk["lang"]
            canonical_chunks.append(entry)

            layer = "names" if section_name in ("nameOfNode", "nodeOfName") else "directClaim"
            flat_shards.append(
                {
                    "id": f"{section_name}:{index}",
                    "section": section_name,
                    "chunkIndex": index,
                    "byteSize": destination.stat().st_size,
                    "digest": {"algorithm": "sha256", "value": digest},
                    "transports": [{"uri": transport_uri(relative, args.base_uri)}],
                    "layers": [layer],
                    "ranges": [],
                }
            )
        canonical_sections[section_name] = {"chunks": canonical_chunks}

    route_input = locate_route_index(legacy, source_base, args.route_index)
    route_relative = "routing/node-route.json"
    route_output = output / route_relative
    copy_deterministic(route_input, route_output)
    route_digest = sha256_file(route_output)

    document = dict(legacy)
    document["layout"] = "zelph-hf-layout/v3"
    document["schemaVersion"] = "zelph-partial-query-manifest-v1"
    document["dataset"] = {
        "id": args.dataset_id,
        "version": args.dataset_version,
        "sourceRevision": legacy.get("sourceRevision", ""),
        "manifestHash": {"algorithm": "sha256", "value": ""},
    }
    document["partitioning"] = {
        "scheme": "zelph-capnp-section-chunks-v1",
        "disjoint": True,
        "overlapSemantics": "none",
        "globalCoverage": bool(args.global_coverage),
    }
    document["representationLayers"] = parse_layers(args.layer)
    document["source"] = {
        "binPath": transport_uri(header_relative, args.base_uri),
        "headerLengthBytes": header_path.stat().st_size,
    }
    document["sections"] = canonical_sections
    document["capabilities"] = {"nodeRouteIndex": True, "features": ["node-route"]}
    document["hfObjects"] = {
        "bin": {"path": transport_uri(header_relative, args.base_uri)},
        "nodeRouteIndex": {"path": transport_uri(route_relative, args.base_uri)},
    }
    document["shards"] = flat_shards
    document["routingIndexes"] = [
        {
            "id": "node-route-v1",
            "key": "node",
            "layers": sorted(name for name, enabled in document["representationLayers"].items() if enabled),
            "exhaustive": True,
            "formatVersion": "zelph-node-route-v1",
            "digest": {"algorithm": "sha256", "value": route_digest},
            "transports": [{"uri": transport_uri(route_relative, args.base_uri)}],
        }
    ]
    document["fallback"] = {"allowedForCompleteness": False, "mode": "reject"}

    blank = serialise_contract(document)
    document["dataset"]["manifestHash"]["value"] = sha256_bytes(blank)
    manifest_output = output / "manifest.json"
    manifest_output.write_bytes(serialise_contract(document))

    checksums: list[str] = []
    for path in sorted(p for p in output.rglob("*") if p.is_file() and p.name != "checksums.sha256"):
        checksums.append(f"{sha256_file(path)}  {path.relative_to(output).as_posix()}")
    (output / "checksums.sha256").write_text("\n".join(checksums) + "\n", encoding="utf-8")

    print(manifest_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
