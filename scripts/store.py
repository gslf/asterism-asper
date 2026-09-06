#!/usr/bin/env python3
"""Maintain an offline Asper store and reconcile curation (Linux, Python 3.11+)."""
import argparse
import json
import sys
from pathlib import Path
from store_admin.archive import export_store, verify_export
from store_admin.erase import erase_store, inspect_store
from store_admin.locking import store
from store_admin.curation import acknowledge, inspect_curation
from store_admin import deferral


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "export", "erase", "curation-inspect", "curation-acknowledge",
                 "curation-sources", "curation-defer", "curation-resume-source"):
        command = commands.add_parser(name)
        command.add_argument("--root", required=True, type=Path)
        if name in ("curation-sources", "curation-defer", "curation-resume-source"):
            command.add_argument("--scope", required=name != "curation-sources")
            command.add_argument("--event", required=name != "curation-sources")
            if name != "curation-sources":
                command.add_argument("--expect-snapshot", required=True)
            if name == "curation-defer":
                command.add_argument("--note", required=True)
        if name == "export":
            command.add_argument("--output", required=True, type=Path)
        elif name == "erase":
            command.add_argument("--expect-snapshot", required=True,
                                 help="snapshot from inspect; binds the destructive operation")
        elif name == "curation-acknowledge":
            command.add_argument("--expect-snapshot", required=True)
            command.add_argument("--note", required=True, help="operator review of the partial effects")
    verify = commands.add_parser("verify")
    verify.add_argument("export", type=Path)
    args = parser.parse_args()
    try:
        if sys.platform != "linux":
            raise ValueError("offline maintenance requires Linux with procfs")
        if args.command == "verify":
            result = verify_export(args.export)
        else:
            with store(args.root, allow_erasing=args.command in ("inspect", "erase")) as (absolute, root):
                if args.command == "inspect":
                    result = inspect_store(root)
                elif args.command == "export":
                    result = export_store(root, absolute, args.output)
                elif args.command == "curation-inspect":
                    result = inspect_curation(root)
                elif args.command == "curation-acknowledge":
                    result = acknowledge(root, args.expect_snapshot, args.note)
                elif args.command == "curation-sources":
                    if (args.scope is None) != (args.event is None):
                        raise ValueError("scope and event must be supplied together")
                    result = deferral.inspect(root, args.scope, args.event)
                elif args.command in ("curation-defer", "curation-resume-source"):
                    result = deferral.update(root, args.expect_snapshot, args.scope, args.event,
                        getattr(args, "note", None), resume=args.command == "curation-resume-source")
                else:
                    result = erase_store(root, args.expect_snapshot)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Store maintenance failed: {error}\n")
    except KeyboardInterrupt:
        parser.exit(130, "Interrupted; inspect the store or verify the export before continuing.\n")
    print(json.dumps(result, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
