"""The prebuilt Vulkan shaders of FSR 3.1's upscaler and frame generation, for
builds without FidelityFX's Windows shader compiler (Linux, the PS5): only the
permutations rexglue's native anti-aliasing and frame generation can ask for.

FidelityFX compiles its shaders at build time with FidelityFX_SC.exe, a Windows
program. The Vulkan library's shaders are SPIR-V in generated headers: per pass
a `<pass>_permutations.h`, with a table from every combination of the pass's
options (the permutation key) to one of its compiled blobs, and a
`<pass>_<md5>.h` per blob. rexglue creates its contexts with fixed flags, so
few keys can be asked for:
- the upscaler (FSR 3): of the 64 combinations of its six options, four (the
  device decides the reprojection's Lanczos lookup table, the pass whether it
  sharpens);
- frame interpolation: one of the eight combinations of its three options;
- optical flow: one of two (FidelityFX never sets its one option).
Each pass also has four variants that the device's subgroup sizes and FP16
support pick (wave32/64, 16-bit or not); those FidelityFX's code uses are kept.

This takes the headers FidelityFX_SC generated in a Windows build of the SDK
with FSR (REXGLUE_ENABLE_FSR=ON: the pinned FidelityFX commit, its own compiler)
and writes to the SDK's thirdparty/fidelityfx-shaders-vk, for every pass of
those three components, its permutation table with every other key pointing at
an empty entry, and the blobs the reachable keys use, unchanged. FSR 2's tables,
which ffx-api links but rexglue never uses (it asks for FSR 3.1), are written
empty. Also written: rexglue_fsr_shaders_vk.h (the create flags the set is for,
which the GPU plugin checks before it creates a context from it), FidelityFX's
licence and a README.

It stops, without writing anything, on an option it doesn't know, on create
flags in the GPU plugin it can't map to options, on backends creating a context
with other flags, and on FidelityFX choosing its permutations from the flags
differently from what this assumes. Run it again after a change of the
FidelityFX commit or of the create flags; the build uses the set as it is.

    python launcher/dev/fsr/fsr_vk_shaders.py [--build <SDK build folder>] [--out <folder>]
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import textwrap
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SDK = os.path.join(ROOT, "third_party", "rexglue-sdk")
SCRIPT = "launcher/dev/fsr/fsr_vk_shaders.py"

# What a create flag means for the upscaler's options. FidelityFX's
# getPipelinePermutationFlags (CHECKED_SOURCE below) derives four of them from
# the context's flags; the other flags don't change the shaders.
OPTION_FLAGS = {
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT": ("FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE", True),
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": (
        "FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS", False),
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": (
        "FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION", True),
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": ("FFX_UPSCALE_ENABLE_DEPTH_INVERTED", True),
}
# Options any rexglue context can use either value of: the device decides the
# Lanczos table, the pass sharpening (the sharpening accumulate pass).
FREE_OPTIONS = {"FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
                "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING"}
NEUTRAL_FLAGS = {"FFX_UPSCALE_ENABLE_AUTO_EXPOSURE", "FFX_UPSCALE_ENABLE_DEBUG_CHECKING",
                 "FFX_UPSCALE_ENABLE_DEPTH_INFINITE", "FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION"}
# FidelityFX's choice of permutation from the context flags, as this script
# assumes it (sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp).
CHECKED_SOURCE = [
    "flags |= (contextFlags & FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE) ? FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT : 0;",
    "flags |= (contextFlags & FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 0 : FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS;",
    "flags |= (contextFlags & FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) ? FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS : 0;",
    "flags |= (contextFlags & FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED) ? FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED : 0;",
    "flags |= (passId == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN) ? FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING : 0;",
    "flags |= (useLut) ? FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE : 0;",
]
PLUGIN_FLAGS = "src/graphics/temporal_aa_fsr.h"
PLUGIN_SOURCES = ["src/graphics/vulkan/temporal_aa.cpp", "src/graphics/d3d12/temporal_aa.cpp"]

# Frame generation: the provider turns the frame generation context's flags into
# frame interpolation's (CHECKED_FG_SOURCE), and frame interpolation chooses its
# permutations from those; optical flow ignores its context's flags.
FG_OPTION_FLAGS = {
    "FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS": (
        "FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS", False),
    "FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS": (
        "FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION", True),
    "FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH": ("FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED", True),
}
FG_FIXED_OPTIONS = {"FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT": 0}
FG_NEUTRAL_FLAGS = {"FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT",
                    "FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE",
                    "FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE",
                    "FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING"}
CHECKED_FG_SOURCE = {
    "ffx-api/src/ffx_provider_framegeneration.cpp": [
        "fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0;",
        "fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) ? FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS : 0;",
        "fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0;",
    ],
    "sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp": [
        "flags |= (contextFlags & FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 0 : FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS;",
        "flags |= (contextFlags & FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS) ? FRAMEINTERPOLATION_SHADER_PERMUTATION_JITTER_MOTION_VECTORS : 0;",
        "flags |= (contextFlags & FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) ? FRAMEINTERPOLATION_SHADER_PERMUTATION_DEPTH_INVERTED : 0;",
    ],
    # Optical flow's permutations don't look at the context's flags (the first
    # parameter has no name).
    "sdk/src/components/opticalflow/ffx_opticalflow.cpp": [
        "static uint32_t getPipelinePermutationFlags(uint32_t, FfxPass, bool fp16, bool force64, bool)",
    ],
}
# And the accessors' keys from those permutation flags (whitespace aside).
CHECKED_FG_KEYS = {
    "ffx_frameinterpolation_shaderblobs.cpp": [
        "key.FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS = FFX_CONTAINS_FLAG(options, FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS);",
        "key.FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS = FFX_CONTAINS_FLAG(options, FRAMEINTERPOLATION_SHADER_PERMUTATION_JITTER_MOTION_VECTORS);",
        "key.FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH = FFX_CONTAINS_FLAG(options, FRAMEINTERPOLATION_SHADER_PERMUTATION_DEPTH_INVERTED);",
    ],
}
PLUGIN_FG_FLAGS = "src/graphics/frame_generation_fsr.h"

# The components whose shaders are kept: table prefix, number of tables (passes
# times four variants), the blob accessor.
COMPONENTS = [
    ("FSR 3", "ffx_fsr3upscaler_", 40, "ffx_fsr3upscaler_shaderblobs.cpp"),
    ("frame interpolation", "ffx_frameinterpolation_", 44, "ffx_frameinterpolation_shaderblobs.cpp"),
    ("optical flow", "ffx_opticalflow_", 28, "ffx_opticalflow_shaderblobs.cpp"),
]
GENERATED = "// Generated by {} from FidelityFX_SC's output (FidelityFX SDK, MIT): the permutations rexglue uses.\n"


def fail(message):
    sys.exit(f"fsr_vk_shaders.py: {message}")


def read_flags(header, constant, prefix, known, neutral):
    text = open(os.path.join(SDK, header), encoding="utf-8").read()
    m = re.search(rf"constexpr uint32_t {constant} = ([^;]*);", text)
    if not m:
        fail(f"no 'constexpr uint32_t {constant} = ...;' in {header}")
    names = set(re.findall(rf"{prefix}\w+", m.group(1)))
    if re.sub(rf"{prefix}\w+|[|\s]", "", m.group(1)):
        fail(f"{header}: {constant} isn't a plain list of {prefix}* ({m.group(1)})")
    unknown = names - neutral - known
    if unknown:
        fail(f"create flags this script doesn't know the effect of: {sorted(unknown)}")
    return names


def create_flags():
    """The FFX_UPSCALE_ENABLE_* flags the GPU plugin creates FSR with (kTaaFsrCreateFlags, which
    both backends must use as they are)."""
    names = read_flags(PLUGIN_FLAGS, "kTaaFsrCreateFlags", "FFX_UPSCALE_ENABLE_",
                       {flag for flag, _ in OPTION_FLAGS.values()}, NEUTRAL_FLAGS)
    for source in PLUGIN_SOURCES:
        code = open(os.path.join(SDK, source), encoding="utf-8").read()
        if "uint32_t flags = kTaaFsrCreateFlags;" not in code:
            fail(f"{source} doesn't create FSR with kTaaFsrCreateFlags")
        added = set(re.findall(r"flags \|= (FFX_UPSCALE_ENABLE_\w+)", code)) - NEUTRAL_FLAGS
        if added:
            fail(f"{source} adds create flags that choose permutations: {sorted(added)}")
    return names


def fg_create_flags():
    """The FFX_FRAMEGENERATION_ENABLE_* flags the GPU plugin creates frame generation with
    (kFgCreateFlags), checked in every plugin source that creates a frame generation context."""
    names = read_flags(PLUGIN_FG_FLAGS, "kFgCreateFlags", "FFX_FRAMEGENERATION_ENABLE_",
                       {flag for flag, _ in FG_OPTION_FLAGS.values()}, FG_NEUTRAL_FLAGS)
    for source in sorted(glob.glob(os.path.join(SDK, "src", "graphics", "**", "*.cpp"), recursive=True)):
        code = open(source, encoding="utf-8").read()
        if "ffxCreateContextDescFrameGeneration " not in code and "ffxCreateContextDescFrameGeneration{" not in code:
            continue
        name = os.path.relpath(source, SDK).replace(os.sep, "/")
        if "flags = kFgCreateFlags;" not in code:
            fail(f"{name} doesn't create frame generation with kFgCreateFlags")
        added = set(re.findall(r"flags \|= (FFX_FRAMEGENERATION_ENABLE_\w+)", code)) - FG_NEUTRAL_FLAGS
        if added:
            fail(f"{name} adds create flags that choose permutations: {sorted(added)}")
    return names


def option_values(flags, fg_flags):
    """Each option's value for these flags (None: either)."""
    values = {name: None for name in FREE_OPTIONS}
    for option, (flag, when_set) in OPTION_FLAGS.items():
        values[option] = int((flag in flags) == when_set)
    for option, (flag, when_set) in FG_OPTION_FLAGS.items():
        values[option] = int((flag in fg_flags) == when_set)
    values.update(FG_FIXED_OPTIONS)
    return values


def parse(path):
    text = open(path, encoding="utf-8").read()
    includes = re.findall(r'^#include "([^"]+)"\s*$', text, re.M)
    key = re.search(r"typedef union (\w+)_PermutationKey \{\s*struct \{(.*?)\};", text, re.S)
    if not key:
        fail(f"{path}: not the permutation header layout this script knows")
    fields = re.findall(r"uint32_t (\w+) : (\d+);", key.group(2))
    table = re.search(r"(static const uint32_t g_\w+_IndirectionTable\[\] = \{)(.*?)(\};)", text, re.S)
    info = re.search(r"(static const \w+_PermutationInfo g_\w+_PermutationInfo\[\] = \{)(.*?)(\};)", text, re.S)
    if not (key and table and info):
        fail(f"{path}: not the permutation header layout this script knows")
    entries = [line.strip() for line in info.group(2).strip().splitlines() if line.strip()]
    return text, includes, fields, table, info, [int(v) for v in re.findall(r"\d+", table.group(2))], entries


def reachable_keys(path, fields, values):
    keys = [0]
    shift = 0
    for name, width in fields:
        width = int(width)
        if name not in values:
            fail(f"{os.path.basename(path)}: unknown permutation option {name}")
        choices = range(1 << width) if values[name] is None else [values[name]]
        keys = [k | (v << shift) for k in keys for v in choices]
        shift += width
    return keys


def entry_blob(entry, includes):
    # "{ g_<pass>_<md5>_size, g_<pass>_<md5>_data, ..." -> "<pass>_<md5>.h"
    blob = re.match(r"\{\s*g_(\w+)_size,", entry).group(1) + ".h"
    if blob not in includes:
        fail(f"{blob} isn't among its table's includes")
    return blob


def git(directory, *args):
    result = subprocess.run(["git", "-C", directory, *args], capture_output=True, text=True)
    if result.returncode:
        fail(f"git {' '.join(args)} in {directory}: {result.stderr.strip()}")
    return result.stdout.strip()


def check_source(ffx):
    """_deps/ffx is the pinned commit, its trees and its files unmodified (as rexglue_fsr.cmake checks)."""
    cmake = open(os.path.join(SDK, "cmake", "rexglue_fsr.cmake"), encoding="utf-8").read()
    pinned = {name: re.search(rf'set\({name} "([0-9a-f]{{40}})"\)', cmake).group(1)
              for name in ("_rexglue_fsr_commit", "_rexglue_fsr_tree_ffx_api", "_rexglue_fsr_tree_sdk")}
    if git(ffx, "rev-parse", "HEAD") != pinned["_rexglue_fsr_commit"]:
        fail(f"{ffx} isn't at {pinned['_rexglue_fsr_commit']}")
    trees = git(ffx, "rev-parse", "HEAD:ffx-api", "HEAD:sdk").split()
    if trees != [pinned["_rexglue_fsr_tree_ffx_api"], pinned["_rexglue_fsr_tree_sdk"]]:
        fail(f"{ffx}: unexpected tree hashes {trees}")
    if subprocess.run(["git", "-C", ffx, "diff", "--quiet", "HEAD", "--", "ffx-api", "sdk"]).returncode:
        fail(f"{ffx}: modified files")
    return pinned["_rexglue_fsr_commit"]


ACCESSORS = os.path.join("sdk", "src", "backends", "shared", "blob_accessors")


def accessor_keys(ffx, accessor):
    """For each table an accessor indexes, the key type it indexes it with."""
    text = open(os.path.join(ffx, ACCESSORS, accessor), encoding="utf-8").read()
    used = {}
    for function in re.split(r"\nstatic FfxShaderBlob ", text)[1:]:
        key_type = re.search(r"(\w+)_PermutationKey key;", function)
        for table in re.findall(r"g_(\w+)_IndirectionTable\[key\.index\]", function):
            if not key_type:
                fail(f"no key type where {accessor} indexes {table}")
            used[table] = key_type.group(1)
    if not used:
        fail(f"{accessor}: no permutation tables indexed (not the layout this script knows)")
    return used


def check_fg_source(ffx):
    for path, lines in CHECKED_FG_SOURCE.items():
        text = open(os.path.join(ffx, path), encoding="utf-8").read()
        for line in lines:
            if line not in text:
                fail(f"FidelityFX no longer chooses frame generation's permutations as this script "
                     f"assumes: missing in {path}\n  {line}")
    for accessor, lines in CHECKED_FG_KEYS.items():
        text = re.sub(r"\s+", " ", open(os.path.join(ffx, ACCESSORS, accessor), encoding="utf-8").read())
        for line in lines:
            if line not in text:
                fail(f"{accessor} no longer builds its keys as this script assumes: missing\n  {line}")


def blob_bytes(path):
    data = re.search(r"_data\[\] = \{(.*?)\};", open(path, encoding="utf-8").read(), re.S).group(1)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", data))


def rewrite(text, table, info, new_table, new_entries, kept_includes):
    # The includes in a fixed order (FidelityFX_SC's varies from run to run).
    lines = [line for line in text[:table.start()].splitlines(keepends=True) if not line.startswith('#include "')]
    includes = "".join(f'#include "{name}"\n' for name in sorted(kept_includes))
    return (GENERATED.format(SCRIPT) + includes + "".join(lines) +
            table.group(1) + "\n" + "".join(f"    {v},\n" for v in new_table) + table.group(3) +
            text[table.end():info.start()] +
            info.group(1) + "\n" + "".join(f"    {e}\n" for e in new_entries) + info.group(3) +
            text[info.end():])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", default=os.path.join(SDK, "out", "build", "win-amd64"),
                        help="the Windows SDK build with FSR")
    parser.add_argument("--out", default=os.path.join(SDK, "thirdparty", "fidelityfx-shaders-vk"))
    args = parser.parse_args()
    source = os.path.join(args.build, "_deps", "ffx-vk", "ffx-api", "sdk", "src", "backends", "shaders", "vk")
    ffx = os.path.join(args.build, "_deps", "ffx")
    sample = open(os.path.join(source, "ffx_fsr3upscaler_accumulate_pass_permutations.h"), encoding="utf-8").read()
    if sample.startswith(GENERATED.format(SCRIPT)):
        fail(f"{args.build} was built from the prebuilt set (REXGLUE_FSR_PREBUILT_SHADERS=ON): use one that "
             "ran FidelityFX_SC")
    # FidelityFX's Vulkan runtime from the same build: what its code uses.
    runtime = open(os.path.join(args.build, "_deps", "ffx-vk", "bin", "amd_fidelityfx_vk.dll"), "rb").read()
    commit = check_source(ffx)

    upscaler = open(os.path.join(ffx, "sdk", "src", "components", "fsr3upscaler", "ffx_fsr3upscaler.cpp"),
                    encoding="utf-8").read()
    for line in CHECKED_SOURCE:
        if line not in upscaler:
            fail(f"FidelityFX no longer chooses permutations as this script assumes: missing\n  {line}")
    check_fg_source(ffx)
    flags = create_flags()
    fg_flags = fg_create_flags()
    values = option_values(flags, fg_flags)

    names = sorted(n for n in os.listdir(source) if n.endswith("_permutations.h"))
    tables = {}  # table file -> (component, the table name its accessor's key type is for)
    for component, prefix, count, accessor in COMPONENTS:
        own = [n for n in names if n.startswith(prefix)]
        if len(own) != count:
            fail(f"unexpected shader set in {source}: {len(own)} {component} tables, not {count}")
        keys_used = accessor_keys(ffx, accessor)
        for n in own:
            tables[n] = (component, keys_used.get(n[:-len("_permutations.h")]))
    fsr2 = [n for n in names if n.startswith("ffx_fsr2_")]
    other = [n for n in names if n not in tables and n not in fsr2]
    if other:
        fail(f"unexpected shader set in {source}: others {other[:3]}")

    outputs = {}
    kept_tables = set()
    blobs = set()
    kept = {component: 0 for component, _, _, _ in COMPONENTS}
    component_blobs = {component: set() for component, _, _, _ in COMPONENTS}
    for name in sorted(tables) + fsr2:
        text, includes, fields, table, info, table_values, entries = parse(os.path.join(source, name))
        table_name = name[:-len("_permutations.h")]
        component, key_type = tables.get(name, (None, None))
        if key_type and key_type != table_name:
            other_fields = parse(os.path.join(source, key_type + "_permutations.h"))[2]
            if other_fields != fields:
                fail(f"the accessor indexes {table_name} with {key_type}'s key, a different layout")
        keys = reachable_keys(name, fields, values) if component else []
        # The kept entries in a fixed order, by blob (FidelityFX_SC's varies).
        used = sorted({table_values[k] for k in keys}, key=lambda i: entry_blob(entries[i], includes))
        # Of a table only what the runtime embeds, all or nothing: FidelityFX's
        # code doesn't use some (the 16-bit ones of passes without FP16, all of
        # frame interpolation's and optical flow's).
        embedded = [blob_bytes(os.path.join(source, entry_blob(entries[i], includes))) in runtime for i in used]
        if embedded and any(embedded) != all(embedded):
            fail(f"{name}: the runtime embeds some of its blobs but not all")
        if not all(embedded):
            keys, used = [], []
        elif component:
            kept_tables.add(name)
        index = {old: new for new, old in enumerate(used)}
        empty = len(used)
        new_table = [index[v] if k in keys else empty for k, v in enumerate(table_values)]
        kept_entries = [entries[i].rstrip(",") + "," for i in used]
        kept_includes = {entry_blob(e, includes) for e in kept_entries}
        outputs[name] = rewrite(text, table, info, new_table, kept_entries + ["{},"], kept_includes)
        blobs |= kept_includes
        if component:
            kept[component] += len(kept_entries)
            component_blobs[component] |= kept_includes
    # The same SPIR-V under several names (Vulkan's wave64 and 16-bit variants are
    # compiled from the same GLSL; FidelityFX_SC names each table's blobs after its
    # pass variant): one header per distinct blob, the tables pointing at it. Two
    # headers are the same blob if they are equal with their names taken out (code,
    # reflection and all).
    def neutral(blob):
        return open(os.path.join(source, blob), encoding="utf-8").read().replace(blob[:-2], "@")
    by_content = {}
    for blob in sorted(blobs, key=lambda b: (len(b), b)):
        by_content.setdefault(neutral(blob), []).append(blob)
    canonical = {blob: group[0] for group in by_content.values() for blob in group}
    for name in list(outputs):
        if name not in tables or name not in kept_tables:
            continue
        text = outputs[name]
        for blob in re.findall(r'^#include "([^"]+)"$', text, re.M):
            if canonical[blob] != blob:
                text = text.replace(f'#include "{blob}"', f'#include "{canonical[blob]}"')
                text = text.replace(f"g_{blob[:-2]}_", f"g_{canonical[blob][:-2]}_")
        # A table may now include one header twice.
        seen, lines = set(), []
        for line in text.splitlines(keepends=True):
            if line.startswith('#include "'):
                if line in seen:
                    continue
                seen.add(line)
            lines.append(line)
        outputs[name] = "".join(lines)
    duplicates = len(blobs) - len(set(canonical.values()))
    component_blobs = {c: {canonical[b] for b in bs} for c, bs in component_blobs.items()}
    blobs = set(canonical.values())
    for blob in blobs:
        # Included by several tables' headers in one file (the blob accessors).
        outputs[blob] = (GENERATED.format(SCRIPT) + "#pragma once\n" +
                         open(os.path.join(source, blob), encoding="utf-8").read())
    for component, _, _, _ in COMPONENTS:
        if not kept[component]:
            fail(f"the runtime embeds none of {component}'s blobs (built without it?)")

    mask = sorted(flag for flag, _ in OPTION_FLAGS.values())
    set_flags = sorted(f for f in flags if f in mask)
    fg_mask = sorted(flag for flag, _ in FG_OPTION_FLAGS.values())
    fg_set_flags = sorted(f for f in fg_flags if f in fg_mask)
    outputs["rexglue_fsr_shaders_vk.h"] = (
        GENERATED.format(SCRIPT) +
        "// The create flags these shaders are for, of those that choose FSR 3's shader permutations\n"
        "// (REXGLUE_FSR_SHADERS_VK_FLAG_MASK) and frame generation's (..._FG_FLAG_MASK): a context\n"
        "// created with others needs shaders that aren't here, and the GPU plugin doesn't create it.\n"
        "#pragma once\n"
        f"#define REXGLUE_FSR_SHADERS_VK_COMMIT \"{commit}\"\n"
        "#define REXGLUE_FSR_SHADERS_VK_FLAG_MASK (" + " | ".join(mask) + ")\n"
        "#define REXGLUE_FSR_SHADERS_VK_FLAGS (" + (" | ".join(set_flags) or "0") + ")\n"
        "#define REXGLUE_FSR_SHADERS_VK_FG_FLAG_MASK (" + " | ".join(fg_mask) + ")\n"
        "#define REXGLUE_FSR_SHADERS_VK_FG_FLAGS (" + (" | ".join(fg_set_flags) or "0") + ")\n")

    def spirv_of(files):
        return sum(len(re.findall(r"0x[0-9a-f]{2}", outputs[b])) for b in files)

    spirv = spirv_of(blobs)
    counts = {component: sum(1 for n in tables if tables[n][0] == component) for component, _, _, _ in COMPONENTS}
    flag_list = ", ".join(f"`{f}`" for f in sorted(flags)) or "none"
    fg_flag_list = ", ".join(f"`{f}`" for f in sorted(fg_flags)) or "none"
    paragraphs = [
        "The SPIR-V of the permutations of AMD FSR 3.1's upscaler and frame generation (FidelityFX "
        f"SDK, commit `{commit}` of the ReXGlue fork, MIT: LICENSE.txt) that rexglue's native "
        "anti-aliasing and frame generation can ask for, in the headers FidelityFX's shader "
        "compiler (FidelityFX_SC.exe, Windows only) generates, for building FidelityFX's Vulkan "
        "runtime where that compiler can't run: Linux and the PS5 (cmake/rexglue_fsr.cmake, "
        "`REXGLUE_FSR_PREBUILT_SHADERS`). Windows builds compile them as FidelityFX does.",
        f"Made by `{SCRIPT}` in mcla-recomp from a Windows build's output, for the create flags "
        f"{flag_list} (FSR) and {fg_flag_list} (frame generation). The {counts['FSR 3']} "
        f"permutation tables of FSR 3's ten passes keep the {kept['FSR 3']} blobs that four keys "
        f"of 64 reach ({len(component_blobs['FSR 3'])} files, {spirv_of(component_blobs['FSR 3']) / 1e6:.2f} MB of "
        f"SPIR-V); the {counts['frame interpolation']} of frame interpolation's eleven passes and "
        f"the {counts['optical flow']} of optical flow's seven keep the {kept['frame interpolation']} "
        f"and {kept['optical flow']} blobs their one key reaches "
        f"({len(component_blobs['frame interpolation'] | component_blobs['optical flow'])} files, "
        f"{spirv_of(component_blobs['frame interpolation'] | component_blobs['optical flow']) / 1e6:.2f} MB). "
        "All unchanged, each distinct blob stored once (the wave64 and 16-bit variants are the "
        "same SPIR-V on Vulkan; their tables point at the same arrays); every other key leads to an "
        "empty entry, and the tables FidelityFX's code "
        "doesn't use (its runtime embeds none of their blobs) are empty. So are FSR 2's, linked "
        "by ffx-api but not used. `rexglue_fsr_shaders_vk.h` names the flags; the GPU plugin "
        "refuses to create a context from this set with others.",
    ]
    outputs["README.md"] = ("# FSR 3.1's upscaler and frame generation shaders for Vulkan, prebuilt\n\n" +
                            "\n\n".join(textwrap.fill(p, 88) for p in paragraphs) + "\n")
    outputs["LICENSE.txt"] = open(os.path.join(ffx, "LICENSE.txt"), encoding="utf-8").read()

    # The output again: in every table the runtime uses, each key rexglue can
    # ask for leads to a blob.
    for name in sorted(kept_tables):
        written = outputs[name]
        new_values = [int(v) for v in re.findall(r"\d+", re.search(
            r"_IndirectionTable\[\] = \{(.*?)\};", written, re.S).group(1))]
        new_entries = [e.strip() for e in re.search(r"_PermutationInfo\[\] = \{(.*?)\};", written,
                                                    re.S).group(1).strip().splitlines() if e.strip()]
        fields = re.findall(r"uint32_t (\w+) : (\d+);", written)
        for k in reachable_keys(name, fields, values):
            if new_entries[new_values[k]] == "{},":
                fail(f"{name}: key {k}, which rexglue can ask for, leads to an empty entry")
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)
    for name, text in outputs.items():
        open(os.path.join(args.out, name), "w", encoding="utf-8", newline="\n").write(text)
    total = sum(os.path.getsize(os.path.join(args.out, n)) for n in os.listdir(args.out))
    print(f"flags {sorted(flags)}, frame generation {sorted(fg_flags)}; "
          + ", ".join(f"{counts[c]} {c} tables ({kept[c]} blobs)" for c, _, _, _ in COMPONENTS)
          + f", {len(blobs)} distinct blob files ({duplicates} duplicates folded; {spirv / 1e6:.2f} MB of SPIR-V), "
          f"{len(fsr2)} empty FSR 2 tables; "
          f"{len(outputs)} files, {total / 1e6:.2f} MB in {args.out}")


if __name__ == "__main__":
    main()
