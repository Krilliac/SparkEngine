'use strict';

const assert = require('assert');
const contract = require('./trusted-ci-aggregate-status.js');

const SHA = 'a'.repeat(40);
const CONTEXT = 'Trusted Exact-Source CI / Aggregate';
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

for (const [label, mutate] of [
    ['missing listed creator', value => { delete value.listed[0].creator; }],
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

console.log('trusted aggregate commit-status response contracts passed');
