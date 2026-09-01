'use strict';

const assert = require('assert');
const contract = require('./trusted-ci-aggregate-status.js');

const SHA = 'a'.repeat(40);
const CONTEXT = 'Trusted Exact-Source CI / Aggregate';
const REPORTER_CONTEXTS = [
    'CI-120 Trusted / Exact Source',
    'CodeQL Trusted / Exact Source'
];
const REPOSITORY = 'Krilliac/SparkEngine';
const REPOSITORY_ID = 1026738544;
const SERVER_URL = 'https://github.com';
const OWN_TARGET = 'https://github.com/Krilliac/SparkEngine/actions/runs/100/attempts/1';
const REPORTER_TARGET = 'https://github.com/Krilliac/SparkEngine/actions/runs/200/attempts/2';
const DESCRIPTION = `Trusted exact-source aggregate is awaiting both reporters for ${SHA.slice(0, 12)}.`;

function creator() {
    return { login: 'github-actions[bot]', id: 41898282 };
}

function createdStatus() {
    return {
        id: 700,
        state: 'pending',
        context: CONTEXT,
        target_url: OWN_TARGET,
        creator: creator()
        // GitHub's create-status response intentionally has no `sha` field.
    };
}

function inventories() {
    return {
        combined: {
            sha: SHA,
            total_count: 2,
            statuses: [
                {
                    id: 701,
                    state: 'pending',
                    context: CONTEXT,
                    target_url: REPORTER_TARGET,
                    description: DESCRIPTION
                    // Combined-status entries intentionally have no creator or sha.
                },
                {
                    id: 702,
                    state: 'success',
                    context: 'CodeQL Trusted / Exact Source',
                    target_url: REPORTER_TARGET,
                    description: 'Trusted CodeQL verified.'
                }
            ]
        },
        listed: [
            {
                id: 701,
                state: 'pending',
                context: CONTEXT,
                target_url: REPORTER_TARGET,
                description: DESCRIPTION,
                creator: creator()
            },
            {
                id: 702,
                state: 'success',
                context: 'CodeQL Trusted / Exact Source',
                target_url: REPORTER_TARGET,
                description: 'Trusted CodeQL verified.',
                creator: creator()
            },
            {
                id: 699,
                state: 'pending',
                context: CONTEXT,
                target_url: OWN_TARGET,
                description: `Trusted exact-source aggregate is evaluating ${SHA.slice(0, 12)}.`,
                creator: creator()
            },
            {
                id: 698,
                state: 'failure',
                context: CONTEXT,
                target_url: OWN_TARGET,
                description: `Trusted exact-source evidence is incomplete for ${SHA.slice(0, 12)}.`,
                creator: creator()
            }
        ]
    };
}

const created = createdStatus();
assert.strictEqual(contract.validateCreatedStatus(created, {
    state: 'pending', context: CONTEXT, targetUrl: OWN_TARGET
}), created);

const liveShape = inventories();
assert.strictEqual(contract.validateLatestPendingStatus({
    combined: liveShape.combined,
    listed: liveShape.listed,
    context: CONTEXT,
    targetSha: SHA,
    ownPendingId: 700,
    ownPendingTargetUrl: OWN_TARGET,
    authorizedTargetUrls: [OWN_TARGET, REPORTER_TARGET]
}).id, 701);

assert.strictEqual(contract.hasCurrentPendingLease({
    combined: liveShape.combined,
    listed: liveShape.listed,
    context: CONTEXT,
    targetSha: SHA,
    ownPendingId: 700,
    ownPendingTargetUrl: OWN_TARGET
}), false);

const ownedLease = inventories();
ownedLease.combined.statuses[0] = {
    id: 700,
    state: 'pending',
    context: CONTEXT,
    target_url: OWN_TARGET,
    description: `Trusted exact-source aggregate is evaluating ${SHA.slice(0, 12)}.`
};
ownedLease.listed.unshift({
    ...ownedLease.combined.statuses[0],
    creator: creator()
});
assert.strictEqual(contract.hasCurrentPendingLease({
    combined: ownedLease.combined,
    listed: ownedLease.listed,
    context: CONTEXT,
    targetSha: SHA,
    ownPendingId: 700,
    ownPendingTargetUrl: OWN_TARGET
}), true);

function readinessInventories() {
    const combined = {
        sha: SHA,
        total_count: 3,
        statuses: [
            { id: 710, state: 'pending', context: CONTEXT },
            { id: 711, state: 'success', context: REPORTER_CONTEXTS[0],
                target_url: `${SERVER_URL}/${REPOSITORY}/actions/runs/900/attempts/2` },
            { id: 712, state: 'failure', context: REPORTER_CONTEXTS[1],
                target_url: `${SERVER_URL}/${REPOSITORY}/actions/runs/901/attempts/1` }
        ]
    };
    return {
        combined,
        listed: combined.statuses.map(status => ({ ...status, creator: creator() }))
    };
}

const readiness = readinessInventories();
assert.strictEqual(contract.hasTerminalReporterSet({
    combined: readiness.combined,
    listed: readiness.listed,
    targetSha: SHA,
    contexts: REPORTER_CONTEXTS
}), true);

function completedReporterEvent() {
    return {
        id: 900,
        workflow_id: 345629369,
        run_number: 65,
        run_attempt: 2,
        name: 'CI-120 Trusted Verifier',
        path: '.github/workflows/ci120-report.yml@refs/heads/Working',
        event: 'workflow_run',
        status: 'completed',
        conclusion: 'success',
        head_branch: 'Working',
        head_sha: SHA,
        repository: { id: REPOSITORY_ID, full_name: REPOSITORY },
        head_repository: { id: REPOSITORY_ID, full_name: REPOSITORY }
    };
}

const exactEventRun = completedReporterEvent();
assert.strictEqual(contract.isExactCompletedReporterEvent({
    combined: readinessInventories().combined,
    targetSha: SHA,
    repository: { id: REPOSITORY_ID, full_name: REPOSITORY },
    serverUrl: SERVER_URL,
    action: 'completed',
    payloadRepository: { id: REPOSITORY_ID, full_name: REPOSITORY },
    workflowRun: exactEventRun,
    currentRun: completedReporterEvent()
}), true);

for (const [label, mutate] of [
    ['stale reporter target', value => { value.workflowRun.id = 899; }],
    ['wrong reporter path', value => { value.workflowRun.path = '.github/workflows/forged.yml'; }],
    ['wrong reporter repository', value => { value.workflowRun.repository.full_name = 'evil/fork'; }],
    ['wrong reporter head repository', value => { value.workflowRun.head_repository.full_name = 'evil/fork'; }],
    ['wrong reporter head branch', value => { value.workflowRun.head_branch = 'Other'; }],
    ['wrong reporter head sha', value => { value.workflowRun.head_sha = 'b'.repeat(40); }],
    ['stale reporter attempt', value => { value.currentRun.run_attempt = 3; }],
    ['wrong workflow id', value => { value.currentRun.workflow_id = 1; }],
    ['nonterminal reporter event', value => { value.action = 'in_progress'; }]
]) {
    const value = {
        combined: readinessInventories().combined,
        targetSha: SHA,
        repository: { id: REPOSITORY_ID, full_name: REPOSITORY },
        serverUrl: SERVER_URL,
        action: 'completed',
        payloadRepository: { id: REPOSITORY_ID, full_name: REPOSITORY },
        workflowRun: completedReporterEvent(),
        currentRun: completedReporterEvent()
    };
    mutate(value);
    assert.strictEqual(contract.isExactCompletedReporterEvent(value), false, label);
}

for (const [label, mutate, expected] of [
    ['reporter pending', value => { value.statuses[1].state = 'pending'; }, false],
    ['reporter missing', value => { value.statuses.splice(1, 1); value.total_count -= 1; }, false],
    ['wrong readiness root sha', value => { value.sha = 'b'.repeat(40); }, 'throw'],
    ['duplicate reporter context', value => { value.statuses.push({ ...value.statuses[1], id: 713 }); value.total_count += 1; }, 'throw'],
    ['unknown reporter state', value => { value.statuses[1].state = 'queued'; }, 'throw']
]) {
    const value = readinessInventories();
    mutate(value.combined);
    value.listed = value.combined.statuses.map(status => ({ ...status, creator: creator() }));
    const evaluate = () => contract.hasTerminalReporterSet({
        combined: value.combined,
        listed: value.listed,
        targetSha: SHA,
        contexts: REPORTER_CONTEXTS
    });
    if (expected === 'throw') {
        assert.throws(evaluate, undefined, label);
    } else {
        assert.strictEqual(evaluate(), expected, label);
    }
}

for (const [label, mutate] of [
    ['missing listed creator', value => { delete value.listed[0].creator; }],
    ['wrong combined root sha', value => { value.combined.sha = 'b'.repeat(40); }],
    ['mismatched inventory id', value => { value.listed[0].id = 999; }],
    ['duplicate combined context', value => { value.combined.statuses.push({ ...value.combined.statuses[0], id: 999 }); value.combined.total_count += 1; }],
    ['unauthorized target', value => { value.combined.statuses[0].target_url = 'https://example.invalid'; }],
    ['terminal state', value => { value.combined.statuses[0].state = 'success'; }],
    ['missing current list status', value => { value.listed.shift(); }],
    ['wrong exact description', value => { value.combined.statuses[0].description = 'forged'; }]
]) {
    const hostile = inventories();
    mutate(hostile);
    assert.throws(() => contract.validateLatestPendingStatus({
        combined: hostile.combined,
        listed: hostile.listed,
        context: CONTEXT,
        targetSha: SHA,
        ownPendingId: 700,
        ownPendingTargetUrl: OWN_TARGET,
        authorizedTargetUrls: [OWN_TARGET, REPORTER_TARGET]
    }), undefined, label);
}

const forgedCreator = createdStatus();
forgedCreator.creator.id = 1;
assert.throws(() => contract.validateCreatedStatus(forgedCreator, {
    state: 'pending', context: CONTEXT, targetUrl: OWN_TARGET
}));

(async () => {
    const successfulCalls = [];
    const published = await contract.publishValidatedTerminalStatus({
        createStatus: async request => {
            successfulCalls.push(request);
            return { data: {
                id: 800,
                state: request.state,
                context: request.context,
                target_url: request.target_url,
                creator: creator()
            } };
        },
        request: { owner: 'Krilliac', repo: 'SparkEngine', sha: SHA },
        state: 'success',
        context: CONTEXT,
        targetUrl: OWN_TARGET,
        successDescription: 'passed',
        pendingDescription: 'uncertain'
    });
    assert.strictEqual(published.state, 'success');
    assert.deepStrictEqual(successfulCalls.map(request => request.state), ['success']);

    const failureCalls = [];
    const publishedFailure = await contract.publishValidatedTerminalStatus({
        createStatus: async request => {
            failureCalls.push(request);
            return { data: {
                id: 803,
                state: request.state,
                context: request.context,
                target_url: request.target_url,
                creator: creator()
            } };
        },
        request: { owner: 'Krilliac', repo: 'SparkEngine', sha: SHA },
        state: 'failure',
        context: CONTEXT,
        targetUrl: OWN_TARGET,
        failureDescription: 'failed',
        pendingDescription: 'uncertain'
    });
    assert.strictEqual(publishedFailure.state, 'failure');
    assert.strictEqual(failureCalls[0].description, 'failed');
    assert.deepStrictEqual(failureCalls.map(request => request.state), ['failure']);

    const malformedFailureCalls = [];
    await assert.rejects(() => contract.publishValidatedTerminalStatus({
        createStatus: async request => {
            malformedFailureCalls.push(request);
            if (request.state === 'failure') {
                return { data: { id: 804, state: 'failure', context: CONTEXT,
                    target_url: OWN_TARGET } };
            }
            return { data: { id: 805, state: 'pending', context: CONTEXT,
                target_url: OWN_TARGET, creator: creator() } };
        },
        request: { owner: 'Krilliac', repo: 'SparkEngine', sha: SHA },
        state: 'failure',
        context: CONTEXT,
        targetUrl: OWN_TARGET,
        failureDescription: 'failed',
        pendingDescription: 'uncertain'
    }));
    assert.deepStrictEqual(
        malformedFailureCalls.map(request => request.state),
        ['failure', 'pending']
    );

    const malformedCalls = [];
    await assert.rejects(() => contract.publishValidatedTerminalStatus({
        createStatus: async request => {
            malformedCalls.push(request);
            if (request.state === 'success') {
                return { data: { id: 801, state: 'success', context: CONTEXT,
                    target_url: OWN_TARGET } };
            }
            return { data: { id: 802, state: 'pending', context: CONTEXT,
                target_url: OWN_TARGET, creator: creator() } };
        },
        request: { owner: 'Krilliac', repo: 'SparkEngine', sha: SHA },
        state: 'success',
        context: CONTEXT,
        targetUrl: OWN_TARGET,
        successDescription: 'passed',
        pendingDescription: 'uncertain'
    }));
    assert.deepStrictEqual(malformedCalls.map(request => request.state), ['success', 'pending']);

    console.log('trusted aggregate commit-status response contracts passed');
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
