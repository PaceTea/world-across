#!/usr/bin/env python3
"""ffx_permutations.py -- the shader permutation headers of AMD's FidelityFX SDK, made on Linux.

The SDK (v1.1.x) builds its shaders with FidelityFX_SC, a Windows program, into headers of SPIR-V
blobs and reflection: for each pass and each combination of its options, the compiled shader and
the names and bindings of every resource it uses -- the Vulkan backend binds resources by those
names. This makes the same headers with glslc: each permutation is compiled twice: optimised, for the
blob itself and the resources it uses, and without optimisation, for those resources' names, which
the optimiser strips.

Only the permutations a context can ask for are compiled. The options the port fixes when it
creates its context (HDR input, motion vector resolution and jitter, depth direction) are taken as
given: every key maps to the permutation with those values and the key's own value of the options
left free. The SDK's wave64 and 16-bit variants are the same shaders under their names, which is
correct, only not as fast where those would have helped.

    ffx_permutations.py --sdk SDK --out DIR --component fsr3upscaler [--glslc glslc] [--jobs N]

SDK is the FidelityFX SDK's sdk/ folder. The headers land in DIR; nothing of the SDK is changed.
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys

# Per component: its pass files, the options in the order the SDK's blob accessors fill the key,
# the values the port fixes (None for an option left free), and the defines every permutation gets.
COMPONENTS = {
    "fsr3upscaler": {
        "dir": "fsr3upscaler",
        "passes": [
            "autogen_reactive", "accumulate", "luma_pyramid", "prepare_reactivity", "prepare_inputs",
            "shading_change", "rcas", "shading_change_pyramid", "luma_instability", "debug_view",
        ],
        "options": [
            ("FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE", None),
            ("FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT", 0),
            ("FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS", 1),
            ("FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS", 0),
            ("FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH", 0),
            ("FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING", None),
        ],
        "defines": [
            "FFX_GPU=1", "FFX_GLSL=1",
            "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
            "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
            "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
            "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
            "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        ],
    },
}

VARIANTS = ["", "_wave64", "_16bit", "_wave64_16bit"]

# SPIR-V, as much of it as reflection needs.
OP_NAME, OP_DECORATE = 5, 71
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE = 25, 26, 27
OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 28, 29, 30, 32
OP_CONSTANT, OP_VARIABLE, OP_TYPE_ACCEL = 43, 59, 5341
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE, DEC_BINDING, DEC_SET = 2, 3, 24, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12
DIM_BUFFER = 5


def spirv_string(words):
    raw = struct.pack("<%dI" % len(words), *words)
    return raw.split(b"\0", 1)[0].decode()


def reflect(spv):
    """The descriptors a module uses: {kind: [(name, binding, count, set)]}."""
    words = struct.unpack("<%dI" % (len(spv) // 4), spv)
    names, decorations, types, constants, variables = {}, {}, {}, {}, []
    at = 5
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 0xFFFF
        ins = words[at + 1:at + count]
        if op == OP_NAME:
            names[ins[0]] = spirv_string(ins[1:])
        elif op == OP_DECORATE:
            decorations.setdefault(ins[0], {})[ins[1]] = ins[2] if len(ins) > 2 else True
        elif op in (OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY,
                    OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_TYPE_ACCEL):
            types[ins[0]] = (op, ins[1:])
        elif op == OP_CONSTANT:
            constants[ins[1]] = ins[2]
        elif op == OP_VARIABLE:
            variables.append((ins[0], ins[1], ins[2]))
        at += count

    found = {k: [] for k in ("cbv", "srv_tex", "uav_tex", "srv_buf", "uav_buf", "sampler", "rtas")}
    for type_id, var_id, storage in variables:
        dec = decorations.get(var_id, {})
        if DEC_BINDING not in dec:
            continue
        op, args = types[type_id]                        # the pointer
        inner, n = args[1], 1
        while types[inner][0] in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            kind, targs = types[inner]
            n *= constants.get(targs[1], 1) if kind == OP_TYPE_ARRAY else 1
            inner = targs[0]
        kind, targs = types[inner]
        name = names.get(var_id) or names.get(inner, "")
        entry = (name, dec[DEC_BINDING], n, dec.get(DEC_SET, 0))
        if kind == OP_TYPE_IMAGE:
            sampled, dim = targs[5], targs[1]
            if dim == DIM_BUFFER:
                found["srv_buf" if sampled == 1 else "uav_buf"].append(entry)
            else:
                found["srv_tex" if sampled == 1 else "uav_tex"].append(entry)
        elif kind == OP_TYPE_SAMPLED_IMAGE:
            found["srv_tex"].append(entry)
        elif kind == OP_TYPE_SAMPLER:
            found["sampler"].append(entry)
        elif kind == OP_TYPE_ACCEL:
            found["rtas"].append(entry)
        elif kind == OP_TYPE_STRUCT:
            tdec = decorations.get(inner, {})
            if storage == SC_UNIFORM and DEC_BLOCK in tdec:
                found["cbv"].append(entry)
            else:
                readonly = DEC_NON_WRITABLE in dec
                found["srv_buf" if readonly else "uav_buf"].append(entry)
    for k in found:
        found[k].sort(key=lambda e: e[1])
    return found


def compile_one(args, source, defines, optimise):
    cmd = [args.glslc, "-fshader-stage=comp", "--target-env=vulkan1.2",
           "-O" if optimise else "-O0", "-I", os.path.join(args.sdk, "include/FidelityFX/gpu"),
           "-I", os.path.join(args.sdk, "include/FidelityFX/gpu", args.comp["dir"]),
           source, "-o", "-"]
    cmd[1:1] = ["-D" + d for d in defines]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode())
        raise SystemExit("ffx_permutations: glslc failed on %s" % source)
    return r.stdout


FIELDS = [("cbv", "constantBuffer"), ("srv_tex", "srvTexture"), ("uav_tex", "uavTexture"),
          ("srv_buf", "srvBuffer"), ("uav_buf", "uavBuffer"), ("sampler", "sampler"),
          ("rtas", "rtAccelerationStructure")]
COUNTS = {"cbv": "numConstantBuffers", "srv_tex": "numSRVTextures", "uav_tex": "numUAVTextures",
          "srv_buf": "numSRVBuffers", "uav_buf": "numUAVBuffers", "sampler": "numSamplers",
          "rtas": "numRTAccelerationStructures"}


def write_info_type(out):
    path = os.path.join(out, "ffx_port_permutation_info.h")
    with open(path, "w") as f:
        f.write("// Generated by tools/ffx_permutations.py: what POPULATE_SHADER_BLOB_FFX reads.\n")
        f.write("#pragma once\n#include <stdint.h>\n\ntypedef struct FfxPortPermutationInfo {\n")
        f.write("    const unsigned char* blobData;\n    uint32_t blobSize;\n")
        for key, field in FIELDS:
            f.write("    uint32_t %s;\n" % COUNTS[key])
            f.write("    const char** %sNames;\n" % field)
            for part in ("Bindings", "Counts", "Spaces"):
                f.write("    const uint32_t* %s%s;\n" % (field, part))
        f.write("} FfxPortPermutationInfo;\n")


def pass_header(args, pass_name):
    comp = args.comp
    base = "ffx_%s_%s_pass" % (args.component, pass_name)
    source = os.path.join(args.sdk, "src/backends/vk/shaders", comp["dir"], base + ".glsl")
    options = comp["options"]
    free = [i for i, (_, fixed) in enumerate(options) if fixed is None]

    # One permutation per combination of the free options.
    perms = []
    for combo in range(1 << len(free)):
        values = [fixed for _, fixed in options]
        for bit, i in enumerate(free):
            values[i] = (combo >> bit) & 1
        defines = comp["defines"] + ["%s=%d" % (name, v) for (name, _), v in zip(options, values)]
        perms.append((values, defines))

    def build(p):
        values, defines = p
        # The resources are the optimised shader's -- the unoptimised one still declares some it
        # never uses, which the SDK's binding tables do not know -- and their names the
        # unoptimised one's, found by set and binding.
        blob = compile_one(args, source, defines, True)
        named = reflect(compile_one(args, source, defines, False))
        names = {(e[3], e[1]): e[0] for kind in named.values() for e in kind}
        used = reflect(blob)
        for kind in used:
            used[kind] = [(names.get((e[3], e[1]), e[0]), e[1], e[2], e[3]) for e in used[kind]]
        return used, blob
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        built = list(pool.map(build, perms))

    # Identical blobs once.
    unique, index_of = [], []
    for refl, blob in built:
        for k, (r2, b2) in enumerate(unique):
            if b2 == blob:
                index_of.append(k)
                break
        else:
            index_of.append(len(unique))
            unique.append((refl, blob))

    lines = ["// Generated by tools/ffx_permutations.py from the FidelityFX SDK's %s.glsl." % base,
             "#pragma once", '#include "ffx_port_permutation_info.h"', ""]
    lines.append("typedef union %s_PermutationKey {" % base)
    lines.append("    struct {")
    for name, _ in options:
        lines.append("        uint32_t %s : 1;" % name)
    lines.append("    };")
    lines.append("    uint32_t index;")
    lines.append("} %s_PermutationKey;" % base)
    lines.append("")
    for u, (refl, blob) in enumerate(unique):
        data = ", ".join("0x%02x" % b for b in blob)
        lines.append("static const unsigned char g_%s_blob%d[] __attribute__((aligned(4))) = { %s };" % (base, u, data))
        for key, field in FIELDS:
            entries = refl[key]
            if not entries:
                continue
            names = ", ".join('"%s"' % e[0] for e in entries)
            lines.append("static const char* g_%s_%s%d_names[] = { %s };" % (base, field, u, names))
            for part, col in (("bindings", 1), ("counts", 2), ("spaces", 3)):
                vals = ", ".join(str(e[col]) for e in entries)
                lines.append("static const uint32_t g_%s_%s%d_%s[] = { %s };" % (base, field, u, part, vals))
    lines.append("")
    lines.append("static const FfxPortPermutationInfo g_%s_PermutationInfo[] = {" % base)
    for u, (refl, blob) in enumerate(unique):
        parts = ["g_%s_blob%d" % (base, u), "%d" % len(blob)]
        for key, field in FIELDS:
            entries = refl[key]
            parts.append(str(len(entries)))
            if entries:
                parts.append("g_%s_%s%d_names" % (base, field, u))
                for part in ("bindings", "counts", "spaces"):
                    parts.append("g_%s_%s%d_%s" % (base, field, u, part))
            else:
                parts += ["0", "0", "0", "0"]
        lines.append("    { %s }," % ", ".join(parts))
    lines.append("};")
    lines.append("")
    # Every key the options can make, to the permutation with the fixed values and its free ones.
    table = []
    for key in range(1 << len(options)):
        combo = 0
        for bit, i in enumerate(free):
            combo |= ((key >> i) & 1) << bit
        table.append(str(index_of[combo]))
    lines.append("static const int32_t g_%s_IndirectionTable[] = { %s };" % (base, ", ".join(table)))
    with open(os.path.join(args.out, base + "_permutations.h"), "w") as f:
        f.write("\n".join(lines) + "\n")
    # The variants, under their own names.
    for v in VARIANTS[1:]:
        with open(os.path.join(args.out, base + v + "_permutations.h"), "w") as f:
            f.write("// Generated by tools/ffx_permutations.py: the %s variant is the plain shader.\n" % v[1:])
            f.write('#pragma once\n#include "%s_permutations.h"\n' % base)
            f.write("#define g_%s%s_PermutationInfo g_%s_PermutationInfo\n" % (base, v, base))
            f.write("#define g_%s%s_IndirectionTable g_%s_IndirectionTable\n" % (base, v, base))
    return len(perms), len(unique)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--component", required=True, choices=sorted(COMPONENTS))
    ap.add_argument("--glslc", default="glslc")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    args.comp = COMPONENTS[args.component]
    os.makedirs(args.out, exist_ok=True)
    write_info_type(args.out)
    for p in args.comp["passes"]:
        n, u = pass_header(args, p)
        print("ffx_permutations: %s %s: %d permutations, %d distinct" % (args.component, p, n, u))


if __name__ == "__main__":
    main()
