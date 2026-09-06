"""Cross the real C receipt, offline lock and restart without a second proposal."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from store_admin import curation
from store_admin.locking import store

parser = argparse.ArgumentParser()
parser.add_argument("--fixture", required=True)
parser.add_argument("--mcp", required=True)
options, remaining = parser.parse_known_args()


class CurationAdministration(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="asper-curation-admin-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "memory"
        result = subprocess.run([options.fixture, str(self.root)], capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 82, result.stderr.decode(errors="replace"))

    def reopen(self):
        return subprocess.run([options.mcp, "--root", str(self.root)], input=b"",
                              capture_output=True, timeout=20)

    def test_reconcile_actual_partial_batch_and_preserve_historical_outcome(self):
        with store(self.root) as (_, root):
            plan = curation.inspect_curation(root)
            self.assertEqual(plan["state"], "requires_reconciliation")
            receipt = plan["receipt"]
            self.assertEqual(receipt["scope"], "receipt")
            self.assertEqual(len(receipt["sources"]), 2)
            self.assertIn("Otters", receipt["proposal"])
            self.assertEqual(receipt["journal_ops_before"], 0)
            note = "Verificato: tè verde 🍵; kept the one persisted insertion."
            curation.acknowledge(root, plan["snapshot"], note)
        result = self.reopen()
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertFalse((self.root / curation.PENDING).exists())
        history = (self.root / "curation.events").read_bytes()
        outcome = curation.decode(history)
        self.assertEqual(outcome["outcome"], "interrupted_acknowledged")
        self.assertEqual(outcome["resolution_note"], note)
        self.assertIsNone(outcome["counts"])
        self.assertEqual(set((self.root / "curated-events.log").read_text().splitlines()), set(receipt["sources"]))
        self.assertIn(b"Otters inhabit freshwater rivers", (self.root / "context.xcdn").read_bytes())
        self.assertEqual(self.reopen().returncode, 0)
        self.assertEqual((self.root / "curation.events").read_bytes(), history)

    def test_stale_store_or_empty_note_cannot_authorize_resolution(self):
        with store(self.root) as (_, root):
            plan = curation.inspect_curation(root)
            with self.assertRaisesRegex(ValueError, "note"):
                curation.acknowledge(root, plan["snapshot"], " ")
            (self.root / "operator-observation.txt").write_text("state changed")
            with self.assertRaisesRegex(ValueError, "snapshot changed"):
                curation.acknowledge(root, plan["snapshot"], "reviewed")
            self.assertEqual(curation.read_pending(root)["outcome"], "prepared")

    def test_failed_replace_and_uncertain_sync_never_requeue_sources(self):
        with store(self.root) as (_, root):
            plan = curation.inspect_curation(root)
            with mock.patch.object(curation.os, "replace", side_effect=OSError("replace failed")):
                with self.assertRaises(OSError):
                    curation.acknowledge(root, plan["snapshot"], "reviewed partial insertion")
            self.assertEqual(curation.read_pending(root)["outcome"], "prepared")
            plan = curation.inspect_curation(root)
            original = os.fsync
            calls = 0

            def uncertain(fd):
                nonlocal calls
                calls += 1
                if calls == 3:
                    raise OSError("uncertain parent sync")
                return original(fd)

            with mock.patch.object(curation.os, "fsync", side_effect=uncertain):
                with self.assertRaises(OSError):
                    curation.acknowledge(root, plan["snapshot"], "reviewed partial insertion")
            self.assertEqual(curation.read_pending(root)["outcome"], "interrupted_acknowledged")
        self.assertEqual(self.reopen().returncode, 0)
        self.assertEqual(curation.decode((self.root / "curation.events").read_bytes())["outcome"],
                         "interrupted_acknowledged")

    def test_codec_rejects_corruption_incomplete_frames_and_invalid_schema(self):
        data = (self.root / curation.PENDING).read_bytes()
        for broken in (b"", data[:-1], data + b"extra", data.replace(b"Otters", b"otters")):
            with self.subTest(broken=broken[:12]), self.assertRaises(ValueError):
                curation.decode(broken)
        receipt = curation.decode(data)
        for key, bad in (("schema", True), ("proposal", "x\0y"), ("scope", "../escape"),
                         ("outcome", "processed"), ("sources", receipt["sources"] * 2),
                         ("journal_ops_before", -1)):
            changed = {**receipt, key: bad}
            with self.subTest(key=key), self.assertRaises(ValueError):
                curation.encode(changed)

    def test_aliases_and_erasure_guard_cannot_be_resolved(self):
        path = self.root / curation.PENDING
        original = path.read_bytes()
        path.unlink()
        target = self.root.parent / "outside"
        target.write_bytes(original)
        path.symlink_to(target)
        with store(self.root) as (_, root), self.assertRaises((OSError, ValueError)):
            curation.inspect_curation(root)
        path.unlink(); path.write_bytes(original)
        (self.root / ".erase.pending").write_text("erasure in progress")
        with self.assertRaisesRegex(ValueError, "erasure"):
            with store(self.root):
                pass

    def test_cli_resolution_is_explicit_and_snapshot_bound(self):
        command = [sys.executable, str(SCRIPTS / "store.py")]
        inspected = subprocess.run(command + ["curation-inspect", "--root", str(self.root)],
                                   capture_output=True, text=True, timeout=20)
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        plan = json.loads(inspected.stdout)
        result = subprocess.run(command + ["curation-acknowledge", "--root", str(self.root),
            "--expect-snapshot", plan["snapshot"], "--note", "Kept the inspected insertion"],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)["effects_replayed"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
