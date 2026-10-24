#!/usr/bin/env python3
"""Read selected Engine source and optional CMakeCache metadata without mutation."""
import argparse
import hashlib
import json
from pathlib import Path
import sys


SOURCE_MARKERS = (
    "CMakeLists.txt",
    "include/fad/engine/Engine.h",
    "include/fad/engine/synth/RenderEngine.h",
)
CACHE_FIELDS = (
    "CMAKE_HOME_DIRECTORY",
    "CMAKE_GENERATOR",
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_COMPILER",
    "CMAKE_OSX_ARCHITECTURES",
    "CMAKE_OSX_DEPLOYMENT_TARGET",
    "FAD_ENGINE_BUILD_TESTS",
    "FAD_ENGINE_BUILD_RENDERER",
    "FAD_ENGINE_BUILD_VST3_DEMO",
    "FAD_ENGINE_BUILD_TEST_VARIANT",
    "FAD_MODEL_1_ENABLE_COMMERCIAL_LICENSE_GATE",
    "FAD_MODEL_1_PLUGIN_TEST_HOOKS",
    "FAD_MODEL_1_COPY_PLUGIN_AFTER_BUILD",
    "FAD_MODEL_1_ENABLE_CODESIGN",
)


def source_root(selected):
    selected = selected.expanduser().resolve(strict=True)
    if not selected.is_dir():
        raise ValueError("source must be an existing directory")
    for candidate in (selected, selected / "engine"):
        if all((candidate / marker).is_file() for marker in SOURCE_MARKERS):
            return candidate.resolve()
    raise ValueError("no independent Engine source at selected directory or its engine child")


def cache_fields(cache):
    # Only the explicit metadata allowlist is emitted; do not dump arbitrary
    # cache entries, environment variables, credentials or private build flags.
    found = {}
    for line in cache.read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        declaration, value = line.split("=", 1)
        key = declaration.split(":", 1)[0]
        if key in CACHE_FIELDS:
            found[key] = value
    return found


def cache_bool(value):
    value = value.strip().upper()
    if value in ("ON", "TRUE", "YES", "1"):
        return True
    if value in ("OFF", "FALSE", "NO", "0"):
        return False
    return None


def inspect(selected_source, selected_build=None):
    core = source_root(Path(selected_source))
    parent = core.parent
    private = parent if (parent / "Source/EngineAdapters").is_dir() else None
    workflow = private / "scripts/fad_audio.sh" if private else None
    result = {
        "status": "SOURCE_OBSERVED",
        "core_source": str(core),
        "private_checkout": str(private) if private else None,
        "private_workflow_available": bool(workflow and workflow.is_file()),
        "documentation_available": bool((core / "docs/README.md").is_file() or (private and (private / "docs/engine/README.md").is_file())),
        "source_marker_sha256": {
            marker: hashlib.sha256((core / marker).read_bytes()).hexdigest()
            for marker in SOURCE_MARKERS
        },
        "source_observation_only": True,
        "binary_provenance_verified": False,
        "build_reuse_verified": False,
        "warnings": [],
    }
    if selected_build is not None:
        build = Path(selected_build).expanduser().resolve(strict=True)
        cache = build / "CMakeCache.txt"
        if not cache.is_file():
            raise ValueError("selected build has no existing CMakeCache.txt")
        fields = cache_fields(cache)
        home = fields.get("CMAKE_HOME_DIRECTORY")
        home_path = Path(home).resolve() if home else None
        if home_path == core:
            relationship = "direct-engine"
        elif private and home_path == private:
            relationship = "private-product"
        else:
            relationship = "consumer-binding-not-established"
            result["warnings"].append({
                "code": "UNPROVEN_BUILD_SOURCE",
                "message": "CMake home is not the selected Core or private root; inspect this consumer's actual inputs/link graph before reuse.",
            })
        result["build"] = {
            "path": str(build),
            "cache_sha256": hashlib.sha256(cache.read_bytes()).hexdigest(),
            "source_relationship": relationship,
            "fields": fields,
        }
        flags = (
            ("FAD_MODEL_1_COPY_PLUGIN_AFTER_BUILD", True, "PLUGIN_COPY_ENABLED"),
            ("FAD_MODEL_1_PLUGIN_TEST_HOOKS", True, "PRODUCT_TEST_HOOKS_ENABLED"),
            ("FAD_MODEL_1_ENABLE_CODESIGN", True, "PRODUCT_SIGNING_ENABLED"),
            ("FAD_MODEL_1_ENABLE_COMMERCIAL_LICENSE_GATE", False, "PRODUCT_LICENSE_GATE_DISABLED"),
        )
        for key, flagged_value, code in flags:
            if key in fields and cache_bool(fields[key]) is flagged_value:
                result["warnings"].append({"code": code, "field": key, "value": fields[key]})
        result["warnings"].append({
            "code": "READ_ONLY_METADATA_NOT_REUSE_APPROVAL",
            "message": "Generator/source metadata does not check active processes, locks, current compiler inputs or binary identity.",
        })
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="Selected private checkout or independent Engine source root")
    parser.add_argument("--build", help="Optional existing CMake build directory; never created or changed")
    args = parser.parse_args()
    try:
        result = inspect(args.source, args.build)
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "ERROR", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
