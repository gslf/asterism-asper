"""Deferral must unblock later sources without acknowledging or deleting the head."""
import argparse
import hashlib
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
from store_admin import deferral, frames
from store_admin.locking import store

parser = argparse.ArgumentParser()
parser.add_argument("--fixture", required=True)
parser.add_argument("--mcp", required=True)
options, remaining = parser.parse_known_args()


class SourceDeferral(unittest.TestCase):
    def run_fixture(self, action):
        result = subprocess.run([options.fixture, action, str(self.root)], capture_output=True,
                                text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="asper-deferral-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "memory"
        seed = self.run_fixture("seed")
        self.assertEqual(seed["result"], "ASPER_OK")
        self.id, self.second = seed["source"], seed["second"]
        self.log = self.root / "scopes/deferred/events.log"
        self.original = self.log.read_bytes()

    def defer(self, note="Review later with a larger transcript budget: tè 🍵"):
        with store(self.root) as (_, root):
            plan = deferral.inspect(root, "deferred", self.id)
            return deferral.update(root, plan["snapshot"], "deferred", self.id, note)

    def policy(self, doc):
        (self.root / deferral.POLICY).write_bytes(frames.checked_encode(json.dumps(doc)))

    def test_later_source_runs_and_resumption_revisits_only_the_unacknowledged_head(self):
        before = self.run_fixture("drain")
        self.assertEqual(before["result"], "ASPER_ERR_LIMIT")
        self.assertEqual(before["calls"], 0)
        self.assertFalse((self.root / "curated-events.log").exists())
        self.assertEqual(self.defer()["state"], "deferred")
        after = self.run_fixture("drain")
        self.assertEqual(after["result"], "ASPER_OK")
        self.assertEqual((after["calls"], after["deferred"], after["queued"]), (1, 1, 0))
        self.assertEqual((self.root / "curated-events.log").read_text().splitlines(), [self.second])
        self.assertEqual(self.run_fixture("drain")["calls"], 0)
        self.assertEqual(self.log.read_bytes(), self.original)
        with store(self.root) as (_, root):
            plan = deferral.inspect(root, "deferred", self.id)
            self.assertEqual(plan["source"]["bytes"], 5000)
            self.assertTrue(plan["source"]["preview_truncated"])
            result = deferral.update(root, plan["snapshot"], "deferred", self.id, None, resume=True)
            self.assertEqual(result["state"], "deferral_removed")
        self.assertEqual(self.run_fixture("drain")["result"], "ASPER_ERR_LIMIT")
        final = self.run_fixture("drain-large")
        self.assertEqual((final["result"], final["calls"], final["deferred"]), ("ASPER_OK", 1, 0))
        self.assertEqual(set((self.root / "curated-events.log").read_text().splitlines()), {self.id, self.second})
        self.assertEqual(self.log.read_bytes(), self.original)
        with store(self.root) as (_, root), self.assertRaisesRegex(ValueError, "already acknowledged"):
            plan = deferral.inspect(root, "deferred", self.id)
            deferral.update(root, plan["snapshot"], "deferred", self.id, "too late")

    def test_snapshot_note_pending_receipt_and_alias_preconditions(self):
        with store(self.root) as (_, root):
            plan = deferral.inspect(root, "deferred", self.id)
            with self.assertRaises(ValueError):
                deferral.update(root, plan["snapshot"], "deferred", self.id, " ")
            (self.root / "external-observation").write_text("changed")
            with self.assertRaisesRegex(ValueError, "snapshot changed"):
                deferral.update(root, plan["snapshot"], "deferred", self.id, "review later")
            (self.root / "curation.pending").write_text("uncertain batch")
            with self.assertRaisesRegex(ValueError, "reconcile"):
                deferral.update(root, plan["snapshot"], "deferred", self.id, "review later")
        (self.root / "curation.pending").unlink()
        self.log.unlink(); self.log.symlink_to(self.root / "context.xcdn")
        with store(self.root) as (_, root), self.assertRaises((OSError, ValueError)):
            deferral.inspect(root, "deferred", self.id)

    def test_runtime_rejects_schema_corruption_and_changed_source_binding(self):
        self.defer()
        path = self.root / deferral.POLICY
        original = path.read_bytes()
        with store(self.root) as (_, root):
            doc = deferral.read(root)
        for key, value in (("id", self.second), ("sequence", 2), ("sha256", "0" * 64),
                           ("scope", "missing"), ("note", "\0"), ("sequence", True)):
            broken = {"schema": 1, "events": [{**doc["events"][0], key: value}]}
            self.policy(broken)
            with self.subTest(key=key, value=value):
                self.assertNotEqual(self.run_fixture("open")["open"], "ASPER_OK")
        for data in (original[:-1], original.replace(b"Review", b"review"),
                     frames.checked_encode('{"schema":1,"schema":1,"events":[]}')):
            path.write_bytes(data)
            self.assertNotEqual(self.run_fixture("open")["open"], "ASPER_OK")
        path.write_bytes(original)
        # A valid newly checksummed frame still cannot change the reviewed input.
        header, payload = self.original.split(b"\n", 1)
        fields = header.split(b" "); n = int(fields[6]) + int(fields[7])
        changed = payload[:n].replace(b"xxx", b"yyy", 1)
        fields[9] = hashlib.sha256(b" ".join(fields[:8]) + changed).hexdigest().encode()
        self.log.write_bytes(b" ".join(fields) + b"\n" + changed + payload[n:])
        self.assertNotEqual(self.run_fixture("open")["open"], "ASPER_OK")

    def test_uncertain_write_is_reported_and_reinspectable(self):
        with store(self.root) as (_, root):
            plan = deferral.inspect(root, "deferred", self.id)
            with mock.patch.object(deferral.os, "replace", side_effect=OSError("replace failed")):
                with self.assertRaises(OSError):
                    deferral.update(root, plan["snapshot"], "deferred", self.id, "review")
            self.assertFalse((self.root / deferral.POLICY).exists())
            plan = deferral.inspect(root, "deferred", self.id)
            real = os.fsync
            calls = 0

            def sync(fd):
                nonlocal calls
                calls += 1
                if calls == 3:
                    raise OSError("uncertain directory sync")
                return real(fd)

            with mock.patch.object(deferral.os, "fsync", side_effect=sync):
                with self.assertRaises(OSError):
                    deferral.update(root, plan["snapshot"], "deferred", self.id, "review")
            self.assertEqual(len(deferral.read(root)["events"]), 1)
        self.assertEqual(self.run_fixture("drain")["result"], "ASPER_OK")

    def test_removed_source_allows_explicit_decision_removal_without_recreation(self):
        self.defer()
        self.log.unlink()
        self.assertNotEqual(self.run_fixture("open")["open"], "ASPER_OK")
        with store(self.root) as (_, root):
            plan = deferral.inspect(root)
            result = deferral.update(root, plan["snapshot"], "deferred", self.id, None, resume=True)
            self.assertEqual(result["state"], "deferral_removed")
        self.assertFalse(self.log.exists())

    def test_multiple_decisions_are_independent_and_survive_checked_export(self):
        from store_admin.archive import export_store, verify_export
        self.defer()
        with store(self.root) as (absolute, root):
            plan = deferral.inspect(root, "deferred", self.second)
            deferral.update(root, plan["snapshot"], "deferred", self.second, "assistant source: review later")
            destination = self.root.parent / "export"
            export_store(root, absolute, destination)
        verify_export(destination)
        original_root = self.root
        self.root = destination / "files"
        try:
            result = self.run_fixture("drain")
            self.assertEqual((result["calls"], result["deferred"], result["queued"]), (0, 2, 0))
            with store(self.root) as (_, root):
                plan = deferral.inspect(root)
                deferral.update(root, plan["snapshot"], "deferred", self.second, None, resume=True)
            result = self.run_fixture("drain")
            self.assertEqual((result["calls"], result["deferred"]), (1, 1))
            self.assertEqual((self.root / "curated-events.log").read_text().splitlines(), [self.second])
        finally:
            self.root = original_root
        self.assertFalse((self.root / "curated-events.log").exists())

    def test_policy_bounds_duplicates_and_source_aliases_fail_closed(self):
        self.defer()
        with store(self.root) as (_, root):
            doc = deferral.read(root)
            with self.assertRaisesRegex(ValueError, "already deferred"):
                plan = deferral.inspect(root, "deferred", self.id)
                deferral.update(root, plan["snapshot"], "deferred", self.id, "again")
        for bad in ({"schema": True, "events": []}, {**doc, "extra": 0},
                    {"schema": 1, "events": doc["events"] * 2},
                    {"schema": 1, "events": doc["events"] * 4097},
                    {"schema": 1, "events": [{**doc["events"][0], "note": "x" * 1025}]}):
            self.policy(bad)
            self.assertNotEqual(self.run_fixture("open")["open"], "ASPER_OK")
            with store(self.root) as (_, root), self.assertRaises(ValueError):
                deferral.read(root)
        self.policy(doc)
        target = self.root.parent / "outside-policy"
        path = self.root / deferral.POLICY
        path.rename(target); path.symlink_to(target)
        self.assertNotEqual(self.run_fixture("open")["open"], "ASPER_OK")
        with store(self.root) as (_, root), self.assertRaises((OSError, ValueError)):
            deferral.read(root)

    def test_cli_and_mcp_observe_deferral_without_model_permissions(self):
        command = [sys.executable, str(SCRIPTS / "store.py")]
        args = ["--root", str(self.root), "--scope", "deferred", "--event", self.id]
        result = subprocess.run(command + ["curation-sources", *args], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        result = subprocess.run(command + ["curation-defer", *args, "--expect-snapshot", plan["snapshot"],
                                           "--note", "Keep this source pending"], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                   "params": {"name": "memory_stats", "arguments": {}}}
        result = subprocess.run([options.mcp, "--root", str(self.root)], input=json.dumps(request)+"\n",
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        response = json.loads(result.stdout)
        self.assertEqual(json.loads(response["result"]["content"][0]["text"])["curation_deferred"], 1)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
