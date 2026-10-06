import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { BundleVerificationError, STATUS_MAX_BYTES, verifyPublicationStatus } from '../verifyBundle.mjs';
import { buildRun, mockGitHubApi, siteStatus } from './evidenceFixture.mjs';

const encoder = new TextEncoder();
const source = 'a'.repeat(40);

function loader(status)
{
    const bytes = encoder.encode(JSON.stringify(status));
    return async (path, maximum) =>
    {
        assert.equal(path, 'status.json');
        assert.equal(maximum, STATUS_MAX_BYTES);
        return bytes;
    };
}

describe('API-bound status document', () =>
{
    test('accepts a successful current Build run with same-commit content', async () =>
    {
        const run = buildRun(source);
        const api = mockGitHubApi(source, [run]);
        const status = await verifyPublicationStatus(loader(siteStatus(source, run)),
                                                     { fetchEvidence: api.fetchEvidence });
        assert.equal(status.state, 'current');
        assert.equal(status.contentCommit, source);
    });

    test('accepts a failed newer Build run while retaining older content', async () =>
    {
        const older = 'b'.repeat(40);
        const failed = buildRun(source, { id: 202, conclusion: 'failure' });
        const api = mockGitHubApi(source, [failed]);
        const status = await verifyPublicationStatus(loader(siteStatus(source, failed, older)),
                                                     { fetchEvidence: api.fetchEvidence });
        assert.equal(status.state, 'blocked');
        assert.equal(status.contentCommit, older);
    });

    test('rejects a forged success against a failed API run', async () =>
    {
        const claimed = buildRun(source);
        const actual = buildRun(source, { conclusion: 'failure' });
        const api = mockGitHubApi(source, [actual]);
        await assert.rejects(verifyPublicationStatus(loader(siteStatus(source, claimed)),
                                                     { fetchEvidence: api.fetchEvidence }),
                             /conclusion differs/);
    });

    test('rejects an older run, rerun, or moved Working head', async () =>
    {
        const claimed = buildRun(source);
        const status = siteStatus(source, claimed);
        const newer = buildRun(source, { id: 203 });
        const api = mockGitHubApi(source, [claimed, newer]);
        await assert.rejects(verifyPublicationStatus(loader(status), { fetchEvidence: api.fetchEvidence }),
                             /newer Build run/);
        api.setRuns([buildRun(source, { attempt: 2 })]);
        await assert.rejects(verifyPublicationStatus(loader(status), { fetchEvidence: api.fetchEvidence }),
                             /identity or conclusion differs/);
        api.setHead('c'.repeat(40));
        await assert.rejects(verifyPublicationStatus(loader(status), { fetchEvidence: api.fetchEvidence }),
                             /Working HEAD differs/);
    });

    test('rejects API denial, malformed status, and oversize status', async () =>
    {
        const run = buildRun(source);
        const api = mockGitHubApi(source, [run]);
        api.failWith(403);
        await assert.rejects(verifyPublicationStatus(loader(siteStatus(source, run)),
                                                     { fetchEvidence: api.fetchEvidence }),
                             /HTTP 403/);
        const malformed = { ...siteStatus(source, run), sourceCommit: 'bad' };
        await assert.rejects(verifyPublicationStatus(loader(malformed),
                                                     { fetchEvidence: mockGitHubApi(source, [run]).fetchEvidence }),
                             BundleVerificationError);
        const oversized = { ...siteStatus(source, run), padding: 'x'.repeat(STATUS_MAX_BYTES) };
        await assert.rejects(verifyPublicationStatus(loader(oversized),
                                                     { fetchEvidence: mockGitHubApi(source, [run]).fetchEvidence }),
                             /exceeds/);
    });
});
