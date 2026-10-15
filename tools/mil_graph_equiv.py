#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Check that one fused MIL program is the chain of two MIL programs.

A staged decode splits a layer into programs that hand tensors to each other
through named lanes. Folding two neighbours into one program removes a call
and keeps the arithmetic only if the fused graph is the chain graph with the
handed-over tensor wired inside. This tool compares the two as labelled
DAGs, on the host, without running anything.

The graph of a program is its ops: op type, literal attributes (BLOBFILE
offsets are masked, tensor names are ignored) and the producers of its
operands. Each node gets a Weisfeiler-Lehman label from its own text and its
producers' labels. Function arguments are labelled by LANE, so a fused
program that swapped two same-shaped inputs, such as q and k, is not equal to
the chain. The chain graph is FIRST followed by SECOND with one SECOND
argument replaced by one FIRST output (--link). The two graphs are equal when
their node label multisets match and every named output lane of the fused
program carries the same label as the chain output it names.

This is strong host evidence of the same ops on the same operands in the
same order of dependence, not a proof of graph isomorphism (colour refinement
cannot tell every pair of different graphs apart). It does not compare the constant bytes behind the BLOBFILE
offsets (compare the ANEC constant sections for that), and it says nothing
about what a chip does with an intermediate that no longer crosses a
program boundary. Only a device run can show that.

usage: mil_graph_equiv.py FUSED.mil FIRST.mil SECOND.mil
         --link SECOND_ARG=FIRST_OUTPUT
         [--lanes-fused A=lane,...] [--lanes-first ...] [--lanes-second ...]
         [--out-fused OUT=lane,...] [--out-first ...] [--out-second ...]

Exit 0 when equal, 1 when not, 2 on a parse problem.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import re
import sys
from pathlib import Path

LINE = re.compile(r"^\s*(?:tensor<)?([^=]*?)>?\s+(\w+)\s*=\s*(\w+)\((.*)\)\[(.*)\];\s*$")
FUNC = re.compile(r"func main<[^>]*>\((.*)\)\s*\{")
OUTPUTS = re.compile(r"\}\s*->\s*\((.*)\);")
OFFSET = re.compile(r"offset\s*=\s*uint64\(\d+\)")
ATTR_NAME = re.compile(r'name\s*=\s*string\("[^"]*"\)\s*,?\s*')
TUPLE = re.compile(r"\(\s*(\w+(?:\s*,\s*\w+)*)\s*\)")
ROUNDS = 64  # the refinement must be at least as deep as the longest chain of ops (44 for the staged fold pair)


class MilError(Exception):
    pass


def split_top(text: str) -> list[str]:
    """Split on commas outside brackets."""
    parts, depth, start = [], 0, 0
    for index, char in enumerate(text):
        if char in "([<":
            depth += 1
        elif char in ")]>":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index])
            start = index + 1
    parts.append(text[start:])
    return [part.strip() for part in parts if part.strip()]


def parse_mil(text: str) -> dict:
    """{'args': [(name, type)], 'outputs': [name], 'nodes': {name: (op, type, literals, refs)}}."""
    args: list[tuple[str, str]] = []
    outputs: list[str] = []
    nodes: dict[str, tuple] = {}
    arg_names: set[str] = set()
    for line in text.splitlines():
        match = FUNC.search(line)
        if match:
            for arg in split_top(match.group(1)):
                typed = re.match(r"tensor<(.*)>\s+(\w+)$", arg)
                if not typed:
                    raise MilError(f"unreadable function argument: {arg!r}")
                args.append((typed.group(2), typed.group(1)))
                arg_names.add(typed.group(2))
            continue
        match = OUTPUTS.search(line)
        if match:
            outputs = [name.strip() for name in match.group(1).split(",")]
            continue
        match = LINE.match(line)
        if not match:
            continue
        kind, name, op, operands, attrs = match.groups()
        literals, refs = [], []
        for operand in split_top(operands):
            key, _, value = operand.partition("=")
            key, value = key.strip(), value.strip()
            names = TUPLE.fullmatch(value)
            if names and all(n.strip() in nodes or n.strip() in arg_names for n in names.group(1).split(",")):
                refs += [(f"{key}#{i}", n.strip()) for i, n in enumerate(names.group(1).split(","))]
            elif re.fullmatch(r"\w+", value) and (value in nodes or value in arg_names):
                refs.append((key, value))
            else:
                literals.append((key, OFFSET.sub("offset=*", value)))
        literals.append(("attrs", OFFSET.sub("offset=*", ATTR_NAME.sub("", attrs).strip())))
        nodes[name] = (op, kind, tuple(sorted(literals)), tuple(refs))
    if not args or not outputs or not nodes:
        raise MilError("no function signature, outputs or ops found")
    return {"args": args, "outputs": outputs, "nodes": nodes}


def label(nodes: dict, args: list, lanes: dict, rounds: int = ROUNDS) -> dict:
    """WL labels for every node and argument. An argument's label is its lane and type."""
    current = {name: f"arg:{lanes.get(name, '?')}:{kind}" for name, kind in args}
    for name, (op, kind, literals, _) in nodes.items():
        current[name] = "|".join([op, kind, str(literals)])
    for _ in range(rounds):
        following = {name: current[name] for name, _ in args}
        for name, (_, _, _, refs) in nodes.items():
            text = "|".join([current[name]] + [f"{key}:{current[ref]}" for key, ref in refs])
            following[name] = hashlib.sha1(text.encode()).hexdigest()
        current = following
    return current


def chain(first: dict, second: dict, link_arg: str, link_output: str, lanes_first: dict, lanes_second: dict):
    """The graph of FIRST then SECOND with SECOND's `link_arg` replaced by FIRST's `link_output`."""
    if link_output not in first["nodes"]:
        raise MilError(f"--link output {link_output!r} is not an op of the first program")
    if link_arg not in {name for name, _ in second["args"]}:
        raise MilError(f"--link argument {link_arg!r} is not an argument of the second program")
    nodes, args, lanes = {}, [], {}
    for name, (op, kind, literals, refs) in first["nodes"].items():
        nodes[f"A_{name}"] = (op, kind, literals, tuple((key, f"A_{ref}") for key, ref in refs))
    for name, kind in first["args"]:
        args.append((f"A_{name}", kind))
        lanes[f"A_{name}"] = lanes_first.get(name, "?")
    for name, (op, kind, literals, refs) in second["nodes"].items():
        mapped = tuple((key, f"A_{link_output}" if ref == link_arg else f"B_{ref}") for key, ref in refs)
        nodes[f"B_{name}"] = (op, kind, literals, mapped)
    for name, kind in second["args"]:
        if name != link_arg:
            args.append((f"B_{name}", kind))
            lanes[f"B_{name}"] = lanes_second.get(name, "?")
    return nodes, args, lanes


def compare(fused: dict, first: dict, second: dict, link: tuple[str, str], lanes: dict, outs: dict) -> dict:
    """lanes and outs: {'fused': {name: lane}, 'first': {...}, 'second': {...}}. Returns a report dict."""
    nodes, args, chain_lanes = chain(first, second, link[0], link[1], lanes.get("first", {}), lanes.get("second", {}))
    fused_labels = label(fused["nodes"], fused["args"], lanes.get("fused", {}))
    chain_labels = label(nodes, args, chain_lanes)
    fused_set = collections.Counter(fused_labels[name] for name in fused["nodes"])
    chain_set = collections.Counter(chain_labels[name] for name in nodes)
    report = {"fused_nodes": len(fused["nodes"]), "chain_nodes": len(nodes),
              "labels_equal": fused_set == chain_set,
              "only_in_fused": sum((fused_set - chain_set).values()), "only_in_chain": sum((chain_set - fused_set).values()),
              "outputs": {}}
    chain_out = {}
    for side, prefix, program in (("first", "A_", first), ("second", "B_", second)):
        for name in program["outputs"]:
            lane = outs.get(side, {}).get(name)
            if lane and not (side == "first" and name == link[1]):
                chain_out[lane] = chain_labels[prefix + name]
    for name in fused["outputs"]:
        lane = outs.get("fused", {}).get(name)
        if lane:
            report["outputs"][lane] = chain_out.get(lane) == fused_labels[name]
    report["outputs_equal"] = bool(report["outputs"]) and all(report["outputs"].values())
    report["equal"] = report["labels_equal"] and report["outputs_equal"] and len(report["outputs"]) == len(chain_out)
    return report


def pairs(text: str | None) -> dict:
    out = {}
    for item in (text or "").split(","):
        if item.strip():
            key, _, value = item.partition("=")
            out[key.strip()] = value.strip()
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    for name in ("fused", "first", "second"):
        parser.add_argument(name)
    parser.add_argument("--link", required=True, help="SECOND_ARG=FIRST_OUTPUT")
    for side in ("fused", "first", "second"):
        parser.add_argument(f"--lanes-{side}", help="ARG=lane,... for the function arguments")
        parser.add_argument(f"--out-{side}", help="OUTPUT=lane,... for the named outputs")
    args = parser.parse_args(argv)
    try:
        link_arg, _, link_output = args.link.partition("=")
        programs = {side: parse_mil(Path(getattr(args, side)).read_text()) for side in ("fused", "first", "second")}
        lanes = {side: pairs(getattr(args, f"lanes_{side}")) for side in programs}
        outs = {side: pairs(getattr(args, f"out_{side}")) for side in programs}
        report = compare(programs["fused"], programs["first"], programs["second"], (link_arg, link_output), lanes, outs)
    except (MilError, OSError) as error:
        print(f"mil_graph_equiv: {error}", file=sys.stderr)
        return 2
    print(f"nodes: fused {report['fused_nodes']}, chain {report['chain_nodes']}")
    print(f"label multisets equal: {report['labels_equal']} (only in fused {report['only_in_fused']}, only in chain {report['only_in_chain']})")
    for lane, same in sorted(report["outputs"].items()):
        print(f"output {lane}: {'equal' if same else 'DIFFERENT'}")
    print("EQUAL" if report["equal"] else "NOT EQUAL")
    return 0 if report["equal"] else 1


if __name__ == "__main__":
    sys.exit(main())
