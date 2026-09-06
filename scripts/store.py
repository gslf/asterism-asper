#!/usr/bin/env python3
"""Inspect, export, verify or erase an offline Asper store (Linux, Python 3.11+)."""
import argparse
import json
import sys
from pathlib import Path
from store_admin.archive import export_store, verify_export
from store_admin.erase import erase_store, inspect_store
from store_admin.locking import store


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "export", "erase"):
        command = commands.add_parser(name)
        command.add_argument("--root", required=True, type=Path)
        if name == "export":
            command.add_argument("--output", required=True, type=Path)
        elif name == "erase":
            command.add_argument("--expect-snapshot", required=True,
                                 help="snapshot from inspect; binds the destructive operation")
    verify = commands.add_parser("verify")
    verify.add_argument("export", type=Path)
    args = parser.parse_args()
    try:
        if sys.platform != "linux":
            raise ValueError("offline maintenance requires Linux with procfs")
        if args.command == "verify":
            result = verify_export(args.export)
        else:
            with store(args.root, allow_erasing=args.command != "export") as (absolute, root):
                if args.command == "inspect":
                    result = inspect_store(root)
                elif args.command == "export":
                    result = export_store(root, absolute, args.output)
                else:
                    result = erase_store(root, args.expect_snapshot)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Store maintenance failed: {error}\n")
    except KeyboardInterrupt:
        parser.exit(130, "Interrupted; inspect the store or verify the export before continuing.\n")
    print(json.dumps(result, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
