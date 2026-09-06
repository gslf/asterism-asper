"""Offline maintenance crosses the real host lock and guards crash recovery."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from store_admin import archive, files
from store_admin.erase import erase_store, inspect_store
from store_admin.locking import store

parser = argparse.ArgumentParser()
parser.add_argument("--mcp", required=True)
options, remaining = parser.parse_known_args()
MCP = options.mcp


def messages(*calls):
    requests = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "store-test", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"}]
    requests += [{"jsonrpc": "2.0", "id": i + 2, "method": "tools/call",
                  "params": {"name": name, "arguments": args}}
                 for i, (name, args) in enumerate(calls)]
    return "".join(json.dumps(r) + "\n" for r in requests)


class StoreMaintenance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="asper-admin-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "memory"
        self.root.mkdir(mode=0o700)
        self.output = self.base / "export"
        (self.root / "manifest.xcdn").write_text("#asper_manifest { store_version: 2 }\n")
        for path in ("context.xcdn", "journal.xcdn", "knowledge.events",
                     "cache/embeddings.bin", "objects/item.bin", "scopes/a/events.log",
                     "context.xcdn.compact.bak", "scopes/a/checkpoint.txt"):
            entry = self.root / path
            entry.parent.mkdir(parents=True, exist_ok=True)
            entry.write_bytes(b"private fixture\0\xff")

    def run_host(self, root=None, text=""):
        return subprocess.run([MCP, "--root", str(root or self.root)], input=text,
                              text=True, capture_output=True, timeout=20)

    def test_export_verifies_every_file_and_detects_tampering(self):
        with store(self.root) as (absolute, fd):
            result = archive.export_store(fd, absolute, self.output)
        self.assertTrue(archive.verify_export(self.output)["verified"])
        manifest = json.loads((self.output / archive.MANIFEST).read_text().splitlines()[0])
        self.assertEqual(result["entries"], manifest["entries"])
        self.assertFalse((self.output / "files/.writer.lock").exists())
        self.assertEqual((self.output / "files/cache/embeddings.bin").read_bytes(), b"private fixture\0\xff")
        (self.output / "files/objects/item.bin").write_bytes(b"private fixturf\0\xff")
        with self.assertRaisesRegex(ValueError, "does not match"):
            archive.verify_export(self.output)

    def test_alias_and_fifo_export_refused_but_erasure_preserves_external_data(self):
        outside = self.base / "outside"
        outside.mkdir()
        secret = outside / "secret"
        secret.write_text("must survive")
        (self.root / "alias").symlink_to(outside, target_is_directory=True)
        os.mkfifo(self.root / "pipe", 0o600)
        os.link(secret, self.root / "linked")
        with store(self.root) as (absolute, fd):
            with self.assertRaisesRegex(ValueError, "aliases"):
                archive.export_store(fd, absolute, self.output)
            plan = inspect_store(fd)
            self.assertTrue(erase_store(fd, plan["snapshot"])["erased"])
        self.assertEqual(secret.read_text(), "must survive")
        self.assertEqual({p.name for p in self.root.iterdir()}, {files.LOCK, files.ERASE})

    def test_stale_snapshot_cannot_authorize_erasure(self):
        with store(self.root) as (_, fd):
            plan = inspect_store(fd)
            (self.root / "context.xcdn").write_text("new information")
            with self.assertRaisesRegex(ValueError, "snapshot changed"):
                erase_store(fd, plan["snapshot"])
        self.assertFalse((self.root / files.ERASE).exists())
        self.assertEqual((self.root / "context.xcdn").read_text(), "new information")

    def test_export_refuses_existing_destination_and_internal_destination(self):
        self.output.mkdir()
        with store(self.root) as (absolute, fd):
            with self.assertRaises(FileExistsError):
                archive.export_store(fd, absolute, self.output)
            with self.assertRaisesRegex(ValueError, "outside"):
                archive.export_store(fd, absolute, self.root / "new")
        self.assertFalse((self.root / "new").exists())

    def test_failed_copy_has_no_completion_manifest(self):
        original = archive.stream_file

        def damage(parent, name, expected, destination=None):
            digest = original(parent, name, expected, destination)
            if destination is not None:
                os.lseek(destination, 0, os.SEEK_SET)
                os.write(destination, b"!")
            return digest

        with store(self.root) as (absolute, fd), mock.patch.object(archive, "stream_file", damage):
            with self.assertRaisesRegex(ValueError, "did not verify"):
                archive.export_store(fd, absolute, self.output)
        self.assertFalse((self.output / archive.MANIFEST).exists())
        with self.assertRaisesRegex(ValueError, "incomplete"):
            archive.verify_export(self.output)

    def test_source_change_after_copy_prevents_export_commit(self):
        original = archive.stream_file

        def mutate(parent, name, expected, destination=None):
            digest = original(parent, name, expected, destination)
            if destination is not None:
                (self.root / "context.xcdn").write_text("changed externally")
            return digest

        with store(self.root) as (absolute, fd), mock.patch.object(archive, "stream_file", mutate):
            with self.assertRaisesRegex(ValueError, "changed"):
                archive.export_store(fd, absolute, self.output)
        self.assertFalse((self.output / archive.MANIFEST).exists())

    def test_untrusted_export_metadata_is_bounded_and_structural(self):
        with store(self.root) as (absolute, fd):
            archive.export_store(fd, absolute, self.output)
        metadata = self.output / archive.MANIFEST
        original = metadata.read_bytes()
        header, rows = original.split(b"\n", 1)
        cases = [original[:-5], header.replace(b'"version":1', b'"version":1,"version":1') + b"\n" + rows,
                 header.replace(b'"entries":', b'"entries":true,"bad":') + b"\n" + rows,
                 b"[" * 1500 + b"]" * 1500 + b"\n", b"x" * (archive.MAX_LINE + 1) + b"\n",
                 header + b'\n{"path":"../../outside","kind":"file"}\n']
        for data in cases:
            with self.subTest(prefix=data[:30]):
                metadata.write_bytes(data)
                with self.assertRaises(ValueError):
                    archive.verify_export(self.output)
        metadata.write_bytes(original)
        self.assertTrue(archive.verify_export(self.output)["verified"])

    def test_quotas_precede_payload_reads_and_symlinked_locks_are_rejected(self):
        with store(self.root) as (_, fd), mock.patch.object(files, "MAX_BYTES", 1):
            with self.assertRaisesRegex(ValueError, "maintenance limit"):
                inspect_store(fd)
        (self.root / files.LOCK).unlink()
        (self.root / files.LOCK).symlink_to(self.root / "context.xcdn")
        with self.assertRaises(OSError), store(self.root):
            self.fail("aliased lock admitted")

    def test_mount_change_and_untrusted_owner_policy_fail_closed(self):
        with store(self.root) as (_, fd):
            with mock.patch.object(files, "mount_id", side_effect=lambda child: 1 if child == fd else 2):
                with self.assertRaisesRegex(ValueError, "mounted|aliased"):
                    inspect_store(fd)
        (self.root / "context.xcdn").chmod(0o666)
        with store(self.root) as (_, fd), self.assertRaisesRegex(ValueError, "writable by others"):
            inspect_store(fd)

    def test_runtime_lock_is_shared_in_both_directions(self):
        shutil.rmtree(self.root)
        with subprocess.Popen([MCP, "--root", str(self.root)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) as host:
            host.stdin.write(messages())
            host.stdin.flush()
            self.assertIn('"result"', host.stdout.readline())
            with self.assertRaisesRegex(ValueError, "busy"), store(self.root):
                self.fail("live host admitted")
            host.stdin.close()
            self.assertEqual(host.wait(timeout=20), 0)
        with store(self.root):
            result = self.run_host()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("BUSY", result.stderr.upper())

    def test_real_memory_export_reopens_with_records_and_source_events(self):
        shutil.rmtree(self.root)
        original = self.run_host(text=messages(
            ("memory_insert", {"section": "context", "content": "A retained observation — 東京"}),
            ("source_append", {"scope": "example", "kind": "user", "text": "Exact source — 東京"})))
        self.assertEqual(original.returncode, 0, original.stderr)
        replies = [json.loads(line) for line in original.stdout.splitlines()]
        self.assertEqual(len(replies), 3)
        for reply in replies:
            self.assertNotIn("error", reply)
            self.assertFalse(reply["result"].get("isError", False))
        with store(self.root) as (absolute, fd):
            archive.export_store(fd, absolute, self.output)
        self.assertTrue(archive.verify_export(self.output)["verified"])
        restored = self.base / "restored"
        shutil.copytree(self.output / "files", restored)
        result = self.run_host(restored, messages(("memory_list", {"section": "context"})))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("A retained observation", result.stdout)
        self.assertTrue((restored / "scopes/example/events.log").is_file())

    def test_process_crash_during_erase_blocks_runtime_and_can_be_resumed(self):
        code = r'''
import os, sys
sys.path.insert(0, sys.argv[1])
from store_admin.locking import store
from store_admin.erase import erase_store, inspect_store
original = os.unlink
def crash(name, **kwargs):
    original(name, **kwargs)
    os._exit(74)
with store(sys.argv[2]) as (_, root):
    plan = inspect_store(root)
    os.unlink = crash
    erase_store(root, plan['snapshot'])
'''
        child = subprocess.run([sys.executable, "-c", code, str(SCRIPTS), str(self.root)], timeout=20)
        self.assertEqual(child.returncode, 74)
        self.assertTrue((self.root / files.ERASE).exists())
        result = self.run_host()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("BUSY", result.stderr.upper())
        with store(self.root, allow_erasing=True) as (_, fd):
            self.assertEqual(inspect_store(fd)["state"], "erasure_pending")
            self.assertTrue(erase_store(fd, inspect_store(fd)["snapshot"])["erased"])
        before = sorted(self.root.iterdir())
        self.assertNotEqual(self.run_host().returncode, 0)
        self.assertEqual(sorted(self.root.iterdir()), before)
        with store(self.root, allow_erasing=True) as (_, fd):
            self.assertEqual(inspect_store(fd)["state"], "empty_blocked")
        with self.assertRaisesRegex(ValueError, "erasure"), store(self.root):
            self.fail("erased store admitted for export")

    def test_new_data_during_erase_stops_and_keeps_guard(self):
        original = os.replace

        def mutate(*args, **kwargs):
            original(*args, **kwargs)
            (self.root / "new-information").write_text("keep for review")

        with store(self.root) as (_, fd), mock.patch.object(os, "replace", mutate):
            with self.assertRaisesRegex(ValueError, "directory changed"):
                erase_store(fd, inspect_store(fd)["snapshot"])
        self.assertEqual((self.root / "new-information").read_text(), "keep for review")
        self.assertTrue((self.root / files.ERASE).exists())

    def test_uncertain_guard_sync_never_acknowledges_erasure(self):
        original = os.fsync
        with store(self.root) as (_, fd):
            plan = inspect_store(fd)

            def fail_guard_sync(target):
                if target == fd and (self.root / files.ERASE).exists():
                    raise OSError("injected guard sync failure")
                original(target)

            with mock.patch.object(os, "fsync", fail_guard_sync), self.assertRaises(OSError):
                erase_store(fd, plan["snapshot"])
        self.assertTrue((self.root / "context.xcdn").exists())
        self.assertTrue((self.root / files.ERASE).exists())
        self.assertNotEqual(self.run_host().returncode, 0)

    def test_cli_inspection_and_erase_need_no_models(self):
        command = [sys.executable, str(SCRIPTS / "store.py")]
        result = subprocess.run(command + ["inspect", "--root", str(self.root)],
                                capture_output=True, text=True, timeout=20, check=True)
        snapshot = json.loads(result.stdout)["snapshot"]
        result = subprocess.run(command + ["erase", "--root", str(self.root),
                                "--expect-snapshot", snapshot], capture_output=True,
                                text=True, timeout=20, check=True)
        self.assertTrue(json.loads(result.stdout)["erased"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
