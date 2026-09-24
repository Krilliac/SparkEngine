"""Exercise publication readiness without treating publication as completed."""
from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "site-data"))
from release_stages import (candidate_readiness_errors, finalization_contract_errors,
                            predecessor_candidate_readiness_errors)
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


def predecessor_candidate():
    contract = candidate()
    contract["readiness"]["gates"][1] = {
        "id": "mixed", "state": "at-risk", "blockingWorkItemIds": ["build", "predecessor-publish"]
    }
    contract["readiness"]["gates"].append({
        "id": "predecessor-publication", "completionPhase": "publication-finalization",
        "state": "at-risk", "blockingWorkItemIds": ["predecessor-publish"],
    })
    profile = contract["readiness"]["releaseProfiles"][0]
    contract["readiness"]["predecessorRelease"] = {
        "id": "v0.9.0-predecessor", "profileId": "test-profile", "state": "candidate",
        "owner": "release-owner", "signOffEvidence": [{"label": "review", "path": "review.json"}],
        "sourceCommitEvidence": {"baselineCommit": "0123456789abcdef0123456789abcdef01234567", "reviewPath": "review.json"},
        "requiredGateIds": ["technical", "mixed", "predecessor-publication"],
        "blockingWorkItemIds": ["build", "predecessor-publish"],
        "qualificationSubstitutions": {},
        "publicationFinalization": {
            "workItemIds": ["predecessor-publish"], "gateId": "predecessor-publication", "environment": "stable-release",
        },
    }
    contract["workItems"].append({
        "id": "predecessor-publish", "status": "in-progress", "dependencies": ["build"],
        "completionPhase": "publication-finalization", "area": "release", "blocking": True,
        "profileApplicability": {profile["id"]: "shared"},
    })
    return contract


class ReleaseStageTests(unittest.TestCase):
    def test_predecessor_replaces_only_v1_terminal_publication(self):
        contract = predecessor_candidate()
        self.assertEqual(predecessor_candidate_readiness_errors(contract), [])

    def test_predecessor_cannot_waive_common_gate_or_work(self):
        contract = predecessor_candidate()
        contract["readiness"]["predecessorRelease"]["requiredGateIds"] = ["predecessor-publication"]
        contract["readiness"]["predecessorRelease"]["blockingWorkItemIds"] = ["predecessor-publish"]
        errors = predecessor_candidate_readiness_errors(contract)
        self.assertTrue(any("requiredGateIds" in error for error in errors))
        self.assertTrue(any("blockingWorkItemIds" in error for error in errors))

    def test_predecessor_requires_real_reviewed_commit_evidence(self):
        contract = predecessor_candidate()
        contract["readiness"]["predecessorRelease"]["sourceCommitEvidence"]["baselineCommit"] = "current-branch"
        self.assertTrue(predecessor_candidate_readiness_errors(contract))

    def test_predecessor_cannot_reuse_v1_finalizer_or_finish_publication(self):
        contract = predecessor_candidate()
        stage = contract["readiness"]["predecessorRelease"]
        stage["publicationFinalization"]["workItemIds"] = ["publish"]
        self.assertTrue(predecessor_candidate_readiness_errors(contract))

    def test_predecessor_substitution_requires_an_evidenced_equivalent(self):
        contract = predecessor_candidate()
        contract["readiness"]["predecessorRelease"]["qualificationSubstitutions"] = {"build": "equivalent"}
        contract["workItems"].append({"id": "equivalent", "status": "done", "dependencies": []})
        self.assertTrue(predecessor_candidate_readiness_errors(contract))

    def test_predecessor_equivalent_does_not_mark_v1_source_done(self):
        contract = predecessor_candidate()
        contract["readiness"]["predecessorRelease"]["qualificationSubstitutions"] = {"build": "equivalent"}
        contract["workItems"].append({
            "id": "equivalent", "status": "open", "dependencies": [],
            "profileApplicability": {"test-profile": "outside"}, "predecessorOnly": True,
        })
        self.assertTrue(predecessor_candidate_readiness_errors(contract))

    def test_actual_ledger_predecessor_can_pass_without_marking_v1_n_minus_one_done(self):
        contract = load_contract()
        contract = copy.deepcopy(contract)
        readiness = contract["readiness"]
        stage = readiness["predecessorRelease"]
        readiness["globalRelease"]["state"] = "candidate"
        profile = readiness["releaseProfiles"][0]
        profile["state"] = "candidate"
        profile["owner"] = "release-owner"
        profile["signOffEvidence"] = [{"label": "review", "path": "review.json"}]
        stage["state"] = "candidate"
        stage["owner"] = "release-owner"
        stage["signOffEvidence"] = [{"label": "review", "path": "review.json"}]
        stage["sourceCommitEvidence"].pop("commit", None)
        stage["sourceCommitEvidence"]["baselineCommit"] = "0123456789abcdef0123456789abcdef01234567"
        items = {item["id"]: item for item in contract["workItems"]}
        for item in items.values():
            item["status"] = "done"
            item["plannedCiJobs"] = []
            item["plannedTestSelectors"] = []
        # The predecessor has explicit equivalents, while the v1 N-1 and
        # publication items remain unfinished and must not be silently promoted.
        for source_id in ("INST-131", "REL-192", "REL-200"):
            items[source_id]["status"] = "open" if source_id != "REL-200" else "blocked"
        items["REL-193"]["status"] = "in-progress"
        for gate in readiness["gates"]:
            gate["state"] = "at-risk" if gate["id"] == "G18" else "passing"
        self.assertEqual(predecessor_candidate_readiness_errors(contract), [])
        contract = predecessor_candidate()
        contract["workItems"][-1]["status"] = "done"
        self.assertTrue(predecessor_candidate_readiness_errors(contract))

    def test_predecessor_malformed_types_fail_closed_without_exceptions(self):
        mutations = (
            lambda c: c["readiness"]["predecessorRelease"].update(profileId={}),
            lambda c: c["readiness"]["predecessorRelease"].update(requiredGateIds=[{}]),
            lambda c: c["readiness"]["predecessorRelease"].update(requiredGateIds=None),
            lambda c: c["readiness"]["predecessorRelease"]["publicationFinalization"].update(workItemIds=[{}]),
            lambda c: c["readiness"]["predecessorRelease"].update(blockingWorkItemIds=[{}]),
        )
        for mutate in mutations:
            contract = predecessor_candidate()
            mutate(contract)
            self.assertTrue(predecessor_candidate_readiness_errors(contract))

    def test_predecessor_retains_global_profile_and_signoff_state_contract(self):
        for path in (
            ("readiness", "globalRelease", "state"),
            ("readiness", "releaseProfiles", 0, "state"),
            ("readiness", "releaseProfiles", 0, "owner"),
            ("readiness", "releaseProfiles", 0, "signOffEvidence"),
        ):
            contract = predecessor_candidate()
            target = contract
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = "blocked" if path[-1] == "state" else ([] if path[-1] == "signOffEvidence" else "unassigned")
            self.assertTrue(predecessor_candidate_readiness_errors(contract), path)

    def test_predecessor_nested_malformed_ledger_values_fail_closed(self):
        mutations = (
            ("work item id", lambda c: c["workItems"][0].update(id={})),
            ("gate id", lambda c: c["readiness"]["gates"][0].update(id={})),
            ("gate blockers", lambda c: c["readiness"]["gates"][0].update(blockingWorkItemIds=[{}])),
            ("work dependencies", lambda c: c["workItems"][0].update(dependencies=[{}])),
            ("profile terminal gate", lambda c: c["readiness"]["releaseProfiles"][0]["publicationFinalization"].update(gateId={})),
            ("predecessor owner", lambda c: c["readiness"]["predecessorRelease"].update(owner={})),
            ("predecessor signoff", lambda c: c["readiness"]["predecessorRelease"].update(signOffEvidence="x")),
        )
        for label, mutate in mutations:
            contract = predecessor_candidate()
            mutate(contract)
            self.assertTrue(predecessor_candidate_readiness_errors(contract), label)

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
