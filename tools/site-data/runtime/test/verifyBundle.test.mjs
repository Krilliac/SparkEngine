// Runtime bundle verification contract (selector: runtime-bundle-validation).
//
// Runs verifyBundle.mjs against a bundle produced by tools/site-data/generate.py
// and against tampered, oversize, malformed, and path-escaping variants of it.
// Every variant the producer-side validator (validate.py
// validate_published_bundle) can see is also materialized on disk and fed to it,
// so the two validators are proven to agree instead of assumed to.
//
// SPARK_SITE_DATA_DIR selects an existing generated publication; otherwise the
// suite generates one with `generate.py --allow-dirty --skip-doc-health`.

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { after, before, describe, test } from 'node:test';
import { fileURLToPath } from 'node:url';

import {
    BUNDLE_MAX_BYTES,
    BundleVerificationError,
    DOCUMENT_PAGE_MAX_BYTES,
    LATEST_MAX_BYTES,
    MAX_JSON_DEPTH,
    MAX_JSON_NODES,
    MAX_JSON_STRING_BYTES,
    SCHEMA_VERSION,
    createFetchLoader,
    parseStrictJson,
    unsafePathReason,
    verifyDocumentPage,
    verifyPublishedBundle,
} from '../verifyBundle.mjs';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..');
const PYTHON = process.env.PYTHON ?? 'python3';
const encoder = new TextEncoder();

let scratch;
let root;
let latest;
let bundle;

function runPython(args, options = {})
{
    return spawnSync(PYTHON, args, { cwd: REPO_ROOT, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, ...options });
}

// A loader over a publication directory, with in-memory overrides so most
// variants need no copy of the 1500-file corpus.
function fileLoader(directory, overrides = new Map())
{
    return async (relative, maximum) =>
    {
        assert.equal(unsafePathReason(relative), null, `verifier asked the loader for unsafe path ${relative}`);
        if (overrides.has(relative))
        {
            const payload = overrides.get(relative);
            if (payload.length > maximum)
            {
                throw new Error(`${relative} exceeds the ${maximum}-byte bound`);
            }
            return payload;
        }
        const absolute = path.join(directory, ...relative.split('/'));
        const stat = fs.lstatSync(absolute);
        if (!stat.isFile())
        {
            throw new Error(`${relative} is not a regular file`);
        }
        if (stat.size > maximum)
        {
            throw new Error(`${relative} exceeds the ${maximum}-byte bound`);
        }
        return new Uint8Array(fs.readFileSync(absolute));
    };
}

// Write a variant to disk for the Python validator: a full copy of the
// publication with the overrides applied (the producer refuses hard links).
function materialize(overrides)
{
    const directory = fs.mkdtempSync(path.join(scratch, 'variant-'));
    fs.cpSync(root, directory, { recursive: true });
    for (const [relative, payload] of overrides)
    {
        const target = path.join(directory, ...relative.split('/'));
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, payload);
    }
    return directory;
}

function pythonVerdict(directory)
{
    const script = [
        'import sys',
        'from pathlib import Path',
        "sys.path.insert(0, 'tools/site-data')",
        'from common import SiteDataError',
        'from validate import validate_published_bundle',
        'try:',
        '    validate_published_bundle(Path(sys.argv[1]))',
        'except SiteDataError as error:',
        '    print(error, file=sys.stderr)',
        '    sys.exit(3)',
    ].join('\n');
    const result = runPython(['-c', script, directory]);
    assert.ok(result.status === 0 || result.status === 3,
              `python validator crashed (status ${result.status}):\n${result.stderr}`);
    return { accepted: result.status === 0, message: result.stderr };
}

async function jsVerdict(overrides, options = {})
{
    try
    {
        await verifyPublishedBundle(fileLoader(root, overrides), options);
        return { accepted: true, message: '' };
    }
    catch (error)
    {
        assert.ok(error instanceof BundleVerificationError, `unexpected error type: ${error.stack}`);
        return { accepted: false, message: error.message };
    }
}

// Both validators must reject the variant, and the runtime must say why.
async function assertBothReject(overrides, expected)
{
    const runtime = await jsVerdict(overrides, { verifyDocuments: true });
    assert.equal(runtime.accepted, false, 'runtime verifier accepted a variant it must reject');
    assert.match(runtime.message, expected);
    const producer = pythonVerdict(materialize(overrides));
    assert.equal(producer.accepted, false, 'validate_published_bundle accepted a variant the runtime rejects');
}

// Compact, like generate.py's bundle, so a rewritten bundle stays inside its
// budget unless a case pads it on purpose.
function jsonBytes(value)
{
    return encoder.encode(JSON.stringify(value));
}

function pointerFor(relative, payload)
{
    return { bytes: payload.length, path: relative, sha256: createHash('sha256').update(payload).digest('hex') };
}

// Replace the bundle with `mutated`, re-pointing latest.json so only the rule
// under test can fail.
function withBundle(mutated, overrides = new Map())
{
    const bundleBytes = jsonBytes(mutated);
    const bundlePath = latest.files.bundle.path;
    overrides.set(bundlePath, bundleBytes);
    overrides.set('latest.json', jsonBytes({ ...latest, files: { ...latest.files, bundle: pointerFor(bundlePath, bundleBytes) } }));
    return overrides;
}

function flipLastByte(payload)
{
    const copy = new Uint8Array(payload);
    copy[copy.length - 2] ^= 0x01;
    return copy;
}

before(async () =>
{
    scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'spark-site-runtime-'));
    if (process.env.SPARK_SITE_DATA_DIR)
    {
        root = path.resolve(process.env.SPARK_SITE_DATA_DIR);
    }
    else
    {
        root = path.join(scratch, '.site-data');
        const generated = runPython(['tools/site-data/generate.py', '--allow-dirty', '--skip-doc-health',
                                     '--output', root]);
        assert.equal(generated.status, 0, `generate.py failed:\n${generated.stdout}\n${generated.stderr}`);
    }
    latest = JSON.parse(fs.readFileSync(path.join(root, 'latest.json'), 'utf8'));
    bundle = JSON.parse(fs.readFileSync(path.join(root, ...latest.files.bundle.path.split('/')), 'utf8'));
});

after(() =>
{
    fs.rmSync(scratch, { recursive: true, force: true });
});

describe('contract constants', () =>
{
    test('match the producer in tools/site-data/common.py', () =>
    {
        const result = runPython([
            '-c',
            "import json, sys; sys.path.insert(0, 'tools/site-data'); import common; " +
                "print(json.dumps([common.SCHEMA_VERSION, common.MAX_JSON_NODES, common.MAX_JSON_DEPTH, " +
                'common.MAX_JSON_STRING_BYTES]))',
        ]);
        assert.equal(result.status, 0, result.stderr);
        assert.deepEqual(JSON.parse(result.stdout),
                         [SCHEMA_VERSION, MAX_JSON_NODES, MAX_JSON_DEPTH, MAX_JSON_STRING_BYTES]);
    });
});

describe('generated publication', () =>
{
    test('is accepted with every document page verified and the displayed SHA bound', async () =>
    {
        const verified = await verifyPublishedBundle(fileLoader(root),
                                                     { verifyDocuments: true, displayedCommit: latest.source.commit });
        assert.equal(verified.commit, latest.source.commit);
        assert.equal(verified.bundle.bundleVersion, verified.commit);
        assert.ok(['current', 'blocked'].includes(verified.publication.state));
        assert.ok(verified.files.site instanceof Uint8Array);
        assert.ok(pythonVerdict(root).accepted, 'producer validator rejects the untouched publication');
    });

    test('verifies a single documentation page on demand', async () =>
    {
        const load = fileLoader(root);
        const verified = await verifyPublishedBundle(load);
        const slug = bundle.docs.documents[0].slug;
        const page = await verifyDocumentPage(load, verified, slug);
        assert.equal(page.document.slug, slug);
        await assert.rejects(verifyDocumentPage(load, verified, 'no-such-slug'), BundleVerificationError);
    });

    test('rejects a displayed SHA that differs from latest.source.commit', async () =>
    {
        const wrong = latest.source.commit.replace(/^./, (c) => (c === '0' ? '1' : '0'));
        const verdict = await jsVerdict(new Map(), { displayedCommit: wrong });
        assert.equal(verdict.accepted, false);
        assert.match(verdict.message, /displayed commit .* differs from latest\.source\.commit/);
    });
});

describe('hash-invalid publications', () =>
{
    test('a flipped byte in a split file', async () =>
    {
        const site = latest.files.site.path;
        const original = await fileLoader(root)(site, Infinity);
        await assertBothReject(new Map([[site, flipLastByte(original)]]), /latest file site SHA-256 differs/);
    });

    test('a flipped byte in the bundle', async () =>
    {
        const bundlePath = latest.files.bundle.path;
        const original = await fileLoader(root)(bundlePath, Infinity);
        await assertBothReject(new Map([[bundlePath, flipLastByte(original)]]), /bundle SHA-256 differs/);
    });

    test('a flipped byte in a documentation page, eagerly and on demand', async () =>
    {
        const document = bundle.docs.documents[0];
        const original = await fileLoader(root)(document.contentPath, Infinity);
        const overrides = new Map([[document.contentPath, flipLastByte(original)]]);
        await assertBothReject(overrides, new RegExp(`document ${document.slug} SHA-256 differs`));

        const load = fileLoader(root, overrides);
        const verified = await verifyPublishedBundle(load);
        await assert.rejects(verifyDocumentPage(load, verified, document.slug), /SHA-256 differs/);
    });

    test('a byte count that disagrees with the pointer', async () =>
    {
        const files = { ...latest.files, metrics: { ...latest.files.metrics, bytes: latest.files.metrics.bytes + 1 } };
        await assertBothReject(new Map([['latest.json', jsonBytes({ ...latest, files })]]),
                               /latest file metrics byte count differs/);
    });
});

describe('oversize publications', () =>
{
    test('latest.json over 32 KiB', async () =>
    {
        const padded = jsonBytes({ ...latest, padding: 'x'.repeat(LATEST_MAX_BYTES) });
        await assertBothReject(new Map([['latest.json', padded]]), /cannot read published latest\.json/);
    });

    test('bundle metadata over 5 MiB with a correct hash', async () =>
    {
        const padded = { ...bundle, padding: 'x'.repeat(BUNDLE_MAX_BYTES) };
        await assertBothReject(withBundle(padded), /bundle cannot be read: .*exceeds/);
    });

    test('a documentation page over 2 MiB with a correct, hash-addressed pointer', async () =>
    {
        const document = bundle.docs.documents[0];
        const page = JSON.parse(new TextDecoder().decode(await fileLoader(root)(document.contentPath, Infinity)));
        const pageBytes = jsonBytes({ ...page, padding: 'x'.repeat(DOCUMENT_PAGE_MAX_BYTES) });
        const digest = createHash('sha256').update(pageBytes).digest('hex');
        const pagePath = document.contentPath.replace(/[0-9a-f]{64}\.json$/, `${digest}.json`);
        const pointer = pointerFor(pagePath, pageBytes);
        const mutated = structuredClone(bundle);
        const target = mutated.docs.documents[0];
        Object.assign(target, { published: pointer, contentPath: pagePath, contentSha256: digest,
                                contentBytes: pageBytes.length });
        mutated.docs.filesBySlug[target.slug] = pointer;
        await assertBothReject(withBundle(mutated, new Map([[pagePath, pageBytes]])),
                               new RegExp(`document ${document.slug} cannot be read: .*exceeds`));
    });
});

describe('malformed publications', () =>
{
    test('truncated latest.json', async () =>
    {
        const bytes = jsonBytes(latest);
        await assertBothReject(new Map([['latest.json', bytes.subarray(0, bytes.length - 10)]]), /Invalid JSON/);
    });

    test('duplicate keys in latest.json', async () =>
    {
        const text = JSON.stringify(latest).replace('{', `{"schemaVersion":${SCHEMA_VERSION},`);
        await assertBothReject(new Map([['latest.json', encoder.encode(text)]]), /duplicate object key/);
    });

    test('an unsupported schemaVersion', async () =>
    {
        await assertBothReject(new Map([['latest.json', jsonBytes({ ...latest, schemaVersion: SCHEMA_VERSION + 1 })]]),
                               /schemaVersion is unsupported/);
    });

    test('a missing source block', async () =>
    {
        const { source: _source, ...withoutSource } = latest;
        await assertBothReject(new Map([['latest.json', jsonBytes(withoutSource)]]),
                               /source commit differs between latest and bundle/);
    });

    test('a missing bundle pointer', async () =>
    {
        const { bundle: _bundle, ...files } = latest.files;
        await assertBothReject(new Map([['latest.json', jsonBytes({ ...latest, files })]]),
                               /bundle pointer is not an object/);
    });

    test('publication metadata that differs between latest and bundle', async () =>
    {
        const flipped = latest.publication.state === 'current' ? 'blocked' : 'current';
        const publication = { ...latest.publication, state: flipped };
        await assertBothReject(new Map([['latest.json', jsonBytes({ ...latest, publication })]]),
                               /publication metadata differs between latest and bundle/);
    });

    test('a current publication without a successful conclusion', async () =>
    {
        const publication = { ...latest.publication, state: 'current', conclusion: 'failure' };
        const overrides = withBundle({ ...bundle, publication });
        const latestWithBundle = JSON.parse(new TextDecoder().decode(overrides.get('latest.json')));
        overrides.set('latest.json', jsonBytes({ ...latestWithBundle, publication }));
        await assertBothReject(overrides, /current publication does not have a successful conclusion/);
    });

    test('a publication state the producer never emits (runtime-only rule)', async () =>
    {
        const publication = { ...latest.publication, state: 'green' };
        const overrides = withBundle({ ...bundle, publication });
        const latestWithBundle = JSON.parse(new TextDecoder().decode(overrides.get('latest.json')));
        overrides.set('latest.json', jsonBytes({ ...latestWithBundle, publication }));
        const verdict = await jsVerdict(overrides);
        assert.equal(verdict.accepted, false);
        assert.match(verdict.message, /publication state "green" is not one of/);
    });
});

describe('path escapes', () =>
{
    for (const [name, escape] of [['parent directory', '../outside.json'],
                                  ['absolute path', '/etc/passwd'],
                                  ['backslash', 'snapshots\\..\\..\\outside.json']])
    {
        test(`a split-file pointer with a ${name}`, async () =>
        {
            const payload = encoder.encode('{}');
            const files = { ...latest.files, site: pointerFor(escape, payload) };
            await assertBothReject(new Map([['latest.json', jsonBytes({ ...latest, files })]]),
                                   /latest file site pointer path is unsafe/);
        });
    }

    test('a documentation pointer that escapes, even when pages are not fetched', async () =>
    {
        const mutated = structuredClone(bundle);
        const target = mutated.docs.documents[0];
        const pointer = { ...target.published, path: '../../outside.json' };
        Object.assign(target, { published: pointer, contentPath: pointer.path });
        mutated.docs.filesBySlug[target.slug] = pointer;
        const overrides = withBundle(mutated);
        const lazy = await jsVerdict(overrides);
        assert.equal(lazy.accepted, false);
        assert.match(lazy.message, /pointer path is unsafe/);
        await assertBothReject(overrides, /pointer path is unsafe/);
    });

    test('URL-changing characters are refused before any fetch', () =>
    {
        for (const unsafe of ['https://evil.example/x.json', 'a/%2e%2e/b.json', 'a?b', 'a#b', 'a//b', './a', ''])
        {
            assert.notEqual(unsafePathReason(unsafe), null, unsafe);
        }
        assert.equal(unsafePathReason(latest.files.site.path), null);
    });
});

describe('strict JSON decoding', () =>
{
    test('rejects a BOM, invalid UTF-8, lone surrogates, non-finite numbers, and excess depth', () =>
    {
        const cases = [
            [Uint8Array.of(0xef, 0xbb, 0xbf, 0x7b, 0x7d), /Invalid JSON/],
            [Uint8Array.of(0x22, 0xff, 0x22), /Invalid UTF-8/],
            [encoder.encode('"\\ud800"'), /invalid Unicode/],
            [encoder.encode('1e999'), /non-finite/],
            [encoder.encode('['.repeat(MAX_JSON_DEPTH + 1) + ']'.repeat(MAX_JSON_DEPTH + 1)), /depth bound/],
            [encoder.encode('{"a":{"b":1,"b":2}}'), /duplicate object key "b"/],
        ];
        for (const [bytes, expected] of cases)
        {
            assert.throws(() => parseStrictJson(bytes, 'fixture', 1024), expected);
        }
        assert.deepEqual(parseStrictJson(encoder.encode('{"a":[{"a":1},{"a":2}],"b":"\\"a\\""}'), 'fixture', 1024),
                         { a: [{ a: 1 }, { a: 2 }], b: '"a"' });
    });
});

describe('fetch loader', () =>
{
    const BASE = 'https://site.example/data/';

    function fakeFetch(load)
    {
        const requested = [];
        const fetchImpl = async (url) =>
        {
            requested.push(url.href);
            const relative = url.pathname.slice(new URL(BASE).pathname.length);
            try
            {
                return new Response(await load(relative, Infinity));
            }
            catch
            {
                return new Response(null, { status: 404 });
            }
        };
        return { fetchImpl, requested };
    }

    test('verifies the generated publication over fetch', async () =>
    {
        const { fetchImpl, requested } = fakeFetch(fileLoader(root));
        const load = createFetchLoader(BASE, { fetch: fetchImpl });
        const verified = await verifyPublishedBundle(load, { displayedCommit: latest.source.commit });
        assert.equal(verified.commit, latest.source.commit);
        assert.ok(requested.every((href) => href.startsWith(BASE)));
    });

    test('caps streamed bodies, refuses escapes, and surfaces HTTP errors', async () =>
    {
        const big = new Uint8Array(LATEST_MAX_BYTES + 1);
        const load = createFetchLoader(BASE, {
            fetch: async (url) => (url.pathname.endsWith('/big.json') ? new Response(big) : new Response(null, { status: 503 })),
        });
        await assert.rejects(load('big.json', LATEST_MAX_BYTES), /exceeds|declares/);
        await assert.rejects(load('../escape.json', 1024), /refusing to fetch/);
        await assert.rejects(load('missing.json', 1024), /HTTP 503/);
        await assert.rejects(verifyPublishedBundle(load), /cannot read published latest\.json: HTTP 503/);
    });
});
