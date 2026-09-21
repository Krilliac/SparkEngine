"""Exercise publication readiness without treating publication as completed."""
from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "site-data"))
from release_stages import candidate_readiness_errors, finalization_contract_errors
from common import load_contract


def candidate():
    return {
        "readiness": {
            "globalRelease": {"state": "candidate"},
            "releaseProfiles": [{
                "id": "test-profile", "state": "candidate", "owner": "release-owner",
                "signOffEvidence": [{"label": "Reviewed candidate", "path": "review.json"}],
                "requiredGateIds": ["technical", "mixed", "publication"],
                "blockingWorkItemIds": ["build", "publish"],
                "publicationFinalization": {
                    "workItemIds": ["publish"], "gateId": "publication", "environment": "stable-release",
                },
            }],
            "gates": [
                {"id": "technical", "state": "passing", "blockingWorkItemIds": ["build"]},
                {"id": "mixed", "state": "at-risk", "blockingWorkItemIds": ["build", "publish"]},
                {"id": "publication", "completionPhase": "publication-finalization", "state": "at-risk", "blockingWorkItemIds": ["publish"]},
            ],
        },
        "workItems": [
            {"id": "build", "status": "done", "dependencies": [], "profileApplicability": {"test-profile": "required"}},
            {"id": "publish", "status": "in-progress", "dependencies": ["build"],
             "completionPhase": "publication-finalization", "area": "release", "blocking": True,
             "profileApplicability": {"test-profile": "shared"}},
        ],
    }


class ReleaseStageTests(unittest.TestCase):
    def test_candidate_keeps_finalizer_and_publication_gates_open_without_mutating(self):
        contract = candidate()
        before = copy.deepcopy(contract)
        self.assertEqual(finalization_contract_errors(contract), [])
        self.assertEqual(candidate_readiness_errors(contract), [])
        self.assertEqual(contract, before)

    def test_technical_failure_cannot_be_exempted(self):
        for gate in ("technical", "mixed"):
            contract = candidate()
            contract["workItems"][0]["status"] = "open"
            self.assertTrue(candidate_readiness_errors(contract), gate)
        contract = candidate()
        contract["readiness"]["gates"][0]["state"] = "blocked"
        self.assertTrue(candidate_readiness_errors(contract))

    def test_only_explicit_finalizer_can_remain_open(self):
        for ids in ([], ["build"], ["publish", "build"], ["publish", "publish"], ["unknown"]):
            contract = candidate()
            contract["readiness"]["releaseProfiles"][0]["publicationFinalization"]["workItemIds"] = ids
            self.assertTrue(finalization_contract_errors(contract), ids)

    def test_terminal_gate_cannot_be_changed_to_a_technical_gate(self):
        contract = candidate()
        contract["readiness"]["releaseProfiles"][0]["publicationFinalization"]["gateId"] = "technical"
        self.assertTrue(finalization_contract_errors(contract))

    def test_arbitrary_metadata_and_environment_are_rejected(self):
        for field, value in (("exemptGateIds", ["technical"]), ("environment", "unprotected")):
            contract = candidate()
            contract["readiness"]["releaseProfiles"][0]["publicationFinalization"][field] = value
            self.assertTrue(finalization_contract_errors(contract))

    def test_missing_or_unclaimed_finalizer_metadata_is_rejected(self):
        contract = candidate()
        del contract["readiness"]["releaseProfiles"][0]["publicationFinalization"]
        self.assertTrue(finalization_contract_errors(contract))
        contract = candidate()
        del contract["workItems"][1]["completionPhase"]
        self.assertTrue(finalization_contract_errors(contract))

    def test_candidate_rejects_premature_completion_or_ready_claim(self):
        for field in ("item", "profile", "global", "gate"):
            contract = candidate()
            if field == "item":
                contract["workItems"][1]["status"] = "done"
            elif field == "profile":
                contract["readiness"]["releaseProfiles"][0]["state"] = "ready"
            elif field == "global":
                contract["readiness"]["globalRelease"]["state"] = "ready"
            else:
                contract["readiness"]["gates"][2]["state"] = "passing"
            self.assertTrue(candidate_readiness_errors(contract), field)

    def test_transitive_dependency_remains_required(self):
        contract = candidate()
        contract["workItems"][0]["dependencies"] = ["hidden"]
        contract["workItems"].append({"id": "hidden", "status": "open", "dependencies": []})
        self.assertTrue(candidate_readiness_errors(contract))

    def test_technical_item_cannot_depend_on_deferred_publication(self):
        contract = candidate()
        contract["workItems"][0]["dependencies"] = ["publish"]
        self.assertTrue(finalization_contract_errors(contract))

    def test_planned_technical_checks_and_unsigned_candidate_are_rejected(self):
        contract = candidate()
        contract["workItems"][0]["plannedCiJobs"] = ["not-implemented"]
        self.assertTrue(candidate_readiness_errors(contract))

    def test_publication_finalizer_cannot_carry_planned_qualification(self):
        contract = candidate()
        contract["workItems"][1]["plannedTestSelectors"] = ["unimplemented-rehearsal"]
        self.assertTrue(finalization_contract_errors(contract))

    def test_repository_rehearsal_is_a_nonexempt_dependency(self):
        contract = load_contract()
        items = {item["id"]: item for item in contract["workItems"]}
        profile = contract["readiness"]["releaseProfiles"][0]
        self.assertEqual(items["REL-190"]["completionPhase"], "qualification")
        self.assertIn("REL-190", items["REL-200"]["dependencies"])
        self.assertIn("REL-190", profile["blockingWorkItemIds"])
        self.assertNotIn("REL-190", profile["publicationFinalization"]["workItemIds"])
        self.assertIn("ReleaseProfileRehearsal_*", items["REL-190"]["plannedTestSelectors"])
        self.assertEqual(items["REL-200"]["plannedTestSelectors"], [])
        self.assertEqual(items["REL-200"]["plannedCiJobs"], [])
        self.assertTrue(any("REL-190" in error for error in candidate_readiness_errors(contract)))
        contract = candidate()
        contract["readiness"]["releaseProfiles"][0]["signOffEvidence"] = []
        self.assertTrue(candidate_readiness_errors(contract))


if __name__ == "__main__":
    unittest.main()
