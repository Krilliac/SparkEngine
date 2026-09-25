// Freshness classification contract for the site-data runtime: every label in
// FRESHNESS_STATES is reachable, and an expired or failed fallback is never
// labelled current.

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import {
    FRESHNESS_STATES,
    MAX_AGE_SECONDS,
    STALE_WHILE_REVALIDATE_SECONDS,
    classifyFreshness,
} from '../freshness.mjs';

const NOW = Date.UTC(2026, 8, 24, 12, 0, 0);

function at(ageSeconds, publicationState, fetchOutcome)
{
    return classifyFreshness({ publicationState, fetchOutcome, verifiedAt: NOW - ageSeconds * 1000, now: NOW });
}

describe('classifyFreshness', () =>
{
    test('uses a five-minute max-age', () =>
    {
        assert.equal(MAX_AGE_SECONDS, 300);
    });

    test('current: verified within max-age, including the boundary', () =>
    {
        assert.equal(at(0, 'current', 'verified').state, 'current');
        const boundary = at(MAX_AGE_SECONDS, 'current', 'verified');
        assert.equal(boundary.state, 'current');
        assert.equal(boundary.revalidate, false);
    });

    test('syncing: past max-age inside the stale-while-revalidate window', () =>
    {
        const pending = at(MAX_AGE_SECONDS + 1, 'current', 'revalidating');
        assert.equal(pending.state, 'syncing');
        assert.equal(pending.revalidate, false, 'a fetch is already in flight');
        const idle = at(MAX_AGE_SECONDS + 1, 'current', 'verified');
        assert.equal(idle.state, 'syncing');
        assert.equal(idle.revalidate, true, 'an expired copy must trigger revalidation');
    });

    test('blocked: the producer published a blocked state', () =>
    {
        assert.equal(at(10, 'blocked', 'verified').state, 'blocked');
        assert.equal(at(MAX_AGE_SECONDS + 1, 'blocked', 'revalidating').state, 'blocked');
    });

    test('stale: revalidation failed or was rejected after max-age', () =>
    {
        for (const outcome of ['failed', 'rejected'])
        {
            for (const state of ['current', 'blocked'])
            {
                const result = at(MAX_AGE_SECONDS + 1, state, outcome);
                assert.equal(result.state, 'stale', `${state}/${outcome}`);
                assert.equal(result.revalidate, true);
            }
        }
    });

    test('stale: past max-age plus stale-while-revalidate, whatever the outcome', () =>
    {
        const beyond = MAX_AGE_SECONDS + STALE_WHILE_REVALIDATE_SECONDS + 1;
        for (const outcome of ['verified', 'revalidating', 'failed', 'rejected'])
        {
            assert.equal(at(beyond, 'current', outcome).state, 'stale', outcome);
            assert.equal(at(beyond, 'blocked', outcome).state, 'stale', outcome);
        }
    });

    test('a failed revalidation inside max-age keeps the fresh label', () =>
    {
        assert.equal(at(60, 'current', 'failed').state, 'current');
        assert.equal(at(60, 'blocked', 'rejected').state, 'blocked');
    });

    test('unavailable: nothing verified is held', () =>
    {
        for (const outcome of ['verified', 'revalidating', 'failed', 'rejected'])
        {
            const result = classifyFreshness({ publicationState: null, fetchOutcome: outcome, verifiedAt: null, now: NOW });
            assert.equal(result.state, 'unavailable');
            assert.equal(result.ageSeconds, null);
            assert.equal(result.revalidate, outcome !== 'revalidating');
        }
    });

    test('a clock that moved backwards clamps age to zero', () =>
    {
        const result = at(-600, 'current', 'verified');
        assert.equal(result.state, 'current');
        assert.equal(result.ageSeconds, 0);
    });

    test('custom windows are honoured', () =>
    {
        const result = classifyFreshness({
            publicationState: 'current',
            fetchOutcome: 'revalidating',
            verifiedAt: NOW - 90_000,
            now: NOW,
            maxAgeSeconds: 60,
            staleWhileRevalidateSeconds: 60,
        });
        assert.equal(result.state, 'syncing');
    });

    test('every declared state is reachable', () =>
    {
        const reached = new Set([
            at(0, 'current', 'verified').state,
            at(MAX_AGE_SECONDS + 1, 'current', 'revalidating').state,
            at(0, 'blocked', 'verified').state,
            at(MAX_AGE_SECONDS + 1, 'current', 'failed').state,
            classifyFreshness({ publicationState: null, fetchOutcome: 'failed', verifiedAt: null, now: NOW }).state,
        ]);
        assert.deepEqual([...reached].sort(), [...FRESHNESS_STATES].sort());
    });

    test('rejects inputs outside the documented domain', () =>
    {
        const valid = { publicationState: 'current', fetchOutcome: 'verified', verifiedAt: NOW, now: NOW };
        assert.throws(() => classifyFreshness({ ...valid, fetchOutcome: 'ok' }), RangeError);
        assert.throws(() => classifyFreshness({ ...valid, publicationState: 'green' }), RangeError);
        assert.throws(() => classifyFreshness({ ...valid, verifiedAt: Number.NaN }), RangeError);
        assert.throws(() => classifyFreshness({ ...valid, now: undefined }), RangeError);
        assert.throws(() => classifyFreshness({ ...valid, maxAgeSeconds: -1 }), RangeError);
    });
});
