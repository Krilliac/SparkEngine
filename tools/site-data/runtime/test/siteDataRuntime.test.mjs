import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { after, before, describe, test } from 'node:test';
import { fileURLToPath } from 'node:url';

import { SiteDataRuntime } from '../siteDataRuntime.mjs';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..');
let scratch;
let root;
let latest;

function loader(overrides = new Map())
{
    return async (relative) =>
    {
        if (overrides.has(relative))
        {
            return overrides.get(relative);
        }
        return new Uint8Array(fs.readFileSync(path.join(root, ...relative.split('/'))));
    };
}

function json(value)
{
    return new TextEncoder().encode(JSON.stringify(value));
}

function pointer(pathName, payload)
{
    return {
        bytes: payload.length,
        path: pathName,
        sha256: createHash('sha256').update(payload).digest('hex'),
    };
}

before(() =>
{
    scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'spark-site-runtime-consumer-'));
    root = process.env.SPARK_SITE_DATA_DIR
        ? path.resolve(process.env.SPARK_SITE_DATA_DIR)
        : path.join(scratch, 'bundle');
    if (!process.env.SPARK_SITE_DATA_DIR)
    {
        const result = spawnSync(process.env.PYTHON ?? 'python3',
                                 ['tools/site-data/generate.py', '--allow-dirty', '--skip-doc-health', '--output', root],
                                 { cwd: REPO_ROOT, encoding: 'utf8' });
        assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
    }
    latest = JSON.parse(fs.readFileSync(path.join(root, 'latest.json'), 'utf8'));
});

after(() =>
{
    fs.rmSync(scratch, { recursive: true, force: true });
});

describe('SiteDataRuntime display boundary', () =>
{
    test('installs only a verified publication and exposes its commit', async () =>
    {
        let now = 1_000_000;
        const runtime = new SiteDataRuntime({ load: loader(), displayedCommit: latest.source.commit, now: () => now });
        assert.equal(runtime.snapshot().state, 'unavailable');
        const result = await runtime.refresh();
        assert.equal(result.state, latest.publication.state === 'blocked' ? 'blocked' : 'current');
        assert.equal(result.commit, latest.source.commit);
        assert.equal(result.bundle.bundleVersion, latest.source.commit);
        now += 10_000;
        assert.equal(runtime.snapshot().state, latest.publication.state === 'blocked' ? 'blocked' : 'current');
    });

    test('keeps the verified fallback and labels rejected refreshes stale', async () =>
    {
        let now = 2_000_000;
        let currentLoader = loader();
        const runtime = new SiteDataRuntime({
            load: (...args) => currentLoader(...args),
            displayedCommit: latest.source.commit,
            now: () => now,
            maxAgeSeconds: 5,
            staleWhileRevalidateSeconds: 5,
        });
        await runtime.refresh();
        const original = fs.readFileSync(path.join(root, 'latest.json'));
        const tampered = new Uint8Array(original);
        tampered[tampered.length - 2] ^= 1;
        currentLoader = loader(new Map([['latest.json', tampered]]));
        now += 6_000;
        const result = await runtime.refresh();
        assert.equal(result.state, 'stale');
        assert.equal(result.banner.startsWith('Stale'), true);
        assert.equal(result.commit, latest.source.commit);
        assert.ok(result.error);
    });

    test('rejects a publication whose displayed SHA differs before exposing data', async () =>
    {
        const wrong = latest.source.commit.replace(/^./, (character) => character === '0' ? '1' : '0');
        const runtime = new SiteDataRuntime({ load: loader(), displayedCommit: wrong });
        const result = await runtime.refresh();
        assert.equal(result.state, 'unavailable');
        assert.match(result.banner, /unavailable/i);
        assert.equal(result.bundle, null);
        assert.ok(result.error);
    });

    test('reports a visible blocked publication state from the verified bundle', async () =>
    {
        const bundlePath = latest.files.bundle.path;
        const originalBundle = JSON.parse(fs.readFileSync(path.join(root, ...bundlePath.split('/')), 'utf8'));
        const publication = { ...originalBundle.publication, state: 'blocked', conclusion: 'failure' };
        const blockedBundleBytes = json({ ...originalBundle, publication });
        const blockedLatest = {
            ...latest,
            publication,
            files: { ...latest.files, bundle: pointer(bundlePath, blockedBundleBytes) },
        };
        const runtime = new SiteDataRuntime({ load: loader(new Map([
            ['latest.json', json(blockedLatest)],
            [bundlePath, blockedBundleBytes],
        ])), displayedCommit: latest.source.commit });
        const result = await runtime.refresh();
        assert.equal(result.state, 'blocked');
        assert.match(result.banner, /blocked/i);
    });
});
