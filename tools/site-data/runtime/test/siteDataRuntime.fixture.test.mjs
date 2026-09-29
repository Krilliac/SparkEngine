import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { describe, test } from 'node:test';

import { SiteDataRuntime } from '../siteDataRuntime.mjs';

const encoder = new TextEncoder();

function publication(commit, {
    state = 'current', conclusion = 'success', lede = 'original', sourceUrl, records,
} = {})
{
    const files = new Map();
    const add = (name, value) =>
    {
        const bytes = encoder.encode(JSON.stringify(value));
        const pointer = { path: name, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') };
        files.set(name, bytes);
        return pointer;
    };
    const publicationState = { state, evidenceCommit: commit, conclusion };
    const document = {
        slug: 'guide', title: 'Guide', sourcePath: 'wiki/Guide.md',
        sourceUrl: sourceUrl ?? `https://github.com/Krilliac/SparkEngine/blob/${commit}/wiki/Guide.md`,
    };
    const page = { schemaVersion: 1, sourceCommit: commit, bundleVersion: commit, document, content: '# Guide' };
    const pageHash = createHash('sha256').update(JSON.stringify(page)).digest('hex');
    const pagePointer = add(`docs/${pageHash}.json`, page);
    Object.assign(document, {
        published: pagePointer, contentPath: pagePointer.path,
        contentSha256: pagePointer.sha256, contentBytes: pagePointer.bytes,
    });
    const search = add('search.json', {
        schemaVersion: 1,
        bundleVersion: commit,
        sourceCommit: commit,
        records: records ?? [{ slug: 'guide', title: 'Guide', sourcePath: 'wiki/Guide.md' }],
    });
    const bundle = {
        schemaVersion: 1,
        bundleVersion: commit,
        source: { commit },
        publication: publicationState,
        site: { home: { hero: { lede } } },
        metrics: [{ id: 'code.totalLines', value: 7 }],
        docs: {
            snapshot: {}, sections: [], documents: [document], filesBySlug: { guide: pagePointer },
            searchPath: search.path, searchSha256: search.sha256, searchBytes: search.bytes,
        },
    };
    const bundlePointer = add('bundle.json', bundle);
    const latest = {
        schemaVersion: 1,
        bundleVersion: commit,
        source: { commit },
        publication: publicationState,
        files: { bundle: bundlePointer, docsSearch: search },
    };
    files.set('latest.json', encoder.encode(JSON.stringify(latest)));
    return { files, latest, bundle };
}

function runtimeFixture(commit, options = {}, now = () => 0)
{
    let current = publication(commit, options);
    const runtime = new SiteDataRuntime({
        load: async (path) => current.files.get(path),
        now,
        maxAgeSeconds: 5,
        staleWhileRevalidateSeconds: 5,
    });
    return { runtime, set(value) { current = value; }, get current() { return current; } };
}

describe('SiteDataRuntime in-memory publication contract', () =>
{
    test('consumes repository-only updates and exposes verified search bytes', async () =>
    {
        const fixture = runtimeFixture('a'.repeat(40), { lede: 'before' });
        const first = await fixture.runtime.refresh();
        assert.equal(first.commit, 'a'.repeat(40));
        assert.equal(first.bundle.site.home.hero.lede, 'before');
        assert.ok(first.files.docsSearch instanceof Uint8Array);

        fixture.set(publication('b'.repeat(40), { lede: 'after' }));
        const second = await fixture.runtime.refresh();
        assert.equal(second.commit, 'b'.repeat(40));
        assert.equal(second.bundle.site.home.hero.lede, 'after');
        assert.equal(second.publication.evidenceCommit, second.commit);
    });

    test('moves from current to blocked on a failed publication', async () =>
    {
        const fixture = runtimeFixture('c'.repeat(40));
        assert.equal((await fixture.runtime.refresh()).state, 'current');
        fixture.set(publication('c'.repeat(40), { state: 'blocked', conclusion: 'failure' }));
        assert.equal((await fixture.runtime.refresh()).state, 'blocked');
        assert.match(fixture.runtime.snapshot().banner, /blocked/i);
    });

    test('a rejected refresh visibly marks fallback stale even inside the cache lifetime', async () =>
    {
        const fixture = runtimeFixture('c'.repeat(40));
        await fixture.runtime.refresh();
        fixture.set({ files: new Map([['latest.json', encoder.encode('{')]]) });
        const view = await fixture.runtime.refresh();
        assert.equal(view.state, 'stale');
        assert.match(view.banner, /Stale/);
        assert.equal(view.commit, 'c'.repeat(40));
    });

    test('retains fallback as stale after malformed refresh and rejects bad hash and oversize latest', async () =>
    {
        let now = 0;
        const fixture = runtimeFixture('d'.repeat(40), {}, () => now);
        await fixture.runtime.refresh();
        const valid = fixture.current;
        const malformed = new Map(fixture.current.files).set('latest.json', encoder.encode('{'));
        fixture.set({ files: malformed });
        now = 6_000;
        const stale = await fixture.runtime.refresh();
        assert.equal(stale.state, 'stale');
        assert.equal(stale.commit, 'd'.repeat(40));

        const badHash = encoder.encode(new TextDecoder().decode(valid.files.get('bundle.json'))
            .replace('original', 'tampered'));
        fixture.set({ files: new Map(valid.files).set('bundle.json', badHash) });
        const rejected = await fixture.runtime.refresh();
        assert.equal(rejected.state, 'stale');
        assert.match(rejected.error.message, /SHA-256 differs/);

        fixture.set({ files: new Map(fixture.current.files).set('latest.json', new Uint8Array(32 * 1024 + 1)) });
        const oversized = await fixture.runtime.refresh();
        assert.equal(oversized.state, 'stale');
    });

    test('rejects hash-consistent missing, duplicate, and mismatched search records', async () =>
    {
        const commit = 'e'.repeat(40);
        const good = { slug: 'guide', title: 'Guide', sourcePath: 'wiki/Guide.md' };
        for (const records of [[], [good, good], [{ ...good, slug: 'absent' }],
                               [{ ...good, sourcePath: 'wiki/Wrong.md' }]])
        {
            const fixture = runtimeFixture(commit, { records });
            const view = await fixture.runtime.refresh();
            assert.equal(view.state, 'unavailable');
            assert.equal(view.bundle, null);
            assert.match(view.error.message, /search/i);
        }
    });

    test('rejects hash-consistent source URLs that are unsafe or name another commit', async () =>
    {
        for (const sourceUrl of ['javascript:alert(1)',
                                 `https://github.com/Krilliac/SparkEngine/blob/${'f'.repeat(40)}/wiki/Guide.md`])
        {
            const fixture = runtimeFixture('a'.repeat(40), { sourceUrl });
            const view = await fixture.runtime.refresh();
            assert.equal(view.state, 'unavailable');
            assert.match(view.error.message, /source URL/i);
        }
    });
});
