#!/usr/bin/env python3

import copy
import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "smoke_test.py"
SPEC = importlib.util.spec_from_file_location("smoke_test", MODULE_PATH)
smoke_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke_test)


def passing_stats():
    return {
        "ready_count": 2,
        "disconnect_count": 1,
        "reconnected": True,
        "key_down_count": 2,
        "key_up_count": 2,
        "key_repeat_count": 0,
        "post_reconnect_key_down_count": 1,
        "post_reconnect_key_up_count": 1,
        "connection_attempt_count": 2,
        "connection_failure_count": 0,
        "connection_failure_statuses": {},
        "stale_event_count": 0,
        "queue_overflow_count": 0,
        "invalid_boot_report_count": 2,
        "matched_report_count": 2,
        "post_reconnect_matched_report_count": 1,
        "report_mismatch_count": 0,
        "unexpected_usb_report_count": 0,
        "pending_bt_reports": [],
        "matched_reports_by_connection": [1, 1],
        "key_downs_by_connection": [1, 1],
        "key_ups_by_connection": [1, 1],
    }


class SummaryTests(unittest.TestCase):
    def summarize(self, stats, required=2):
        return smoke_test.make_summary(
            copy.deepcopy(stats), "start", "end", False, required
        )

    def test_complete_two_connection_run_passes(self):
        summary = self.summarize(passing_stats())
        self.assertEqual(summary["result"], "PASS")
        self.assertTrue(summary["criteria"]["reports_forwarded_in_every_connection"])

    def test_report_mismatch_fails(self):
        stats = passing_stats()
        stats["report_mismatch_count"] = 1
        self.assertEqual(self.summarize(stats)["result"], "INCOMPLETE")

    def test_queue_overflow_fails(self):
        stats = passing_stats()
        stats["queue_overflow_count"] = 1
        self.assertEqual(self.summarize(stats)["result"], "INCOMPLETE")

    def test_every_required_connection_needs_input_and_reports(self):
        stats = passing_stats()
        stats["ready_count"] = 3
        stats["disconnect_count"] = 2
        stats["matched_reports_by_connection"] = [1, 1, 0]
        stats["key_downs_by_connection"] = [1, 1, 1]
        stats["key_ups_by_connection"] = [1, 1, 1]
        self.assertEqual(self.summarize(stats, required=3)["result"], "INCOMPLETE")

    def test_pending_bt_report_fails(self):
        stats = passing_stats()
        stats["pending_bt_reports"] = [("00", "04,00,00,00,00,00")]
        self.assertEqual(self.summarize(stats)["result"], "INCOMPLETE")


if __name__ == "__main__":
    unittest.main()
