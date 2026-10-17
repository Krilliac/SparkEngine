import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { mountSiteDataRuntime, renderSiteDataView } from '../browserRuntime.mjs';

function element(attributes = {})
{
    return {
        attributes,
        dataset: {},
        textContent: '',
        children: [],
        firstChild: null,
        getAttribute(name) { return this.attributes[name] ?? null; },
        appendChild(child) { this.children.push(child); this.firstChild = this.children[0] ?? null; },
        removeChild() { this.children.shift(); this.firstChild = this.children[0] ?? null; },
    };
}

function root()
{
    const nodes = new Map([
        ['[data-site-banner]', element()],
        ['[data-site-commit]', element()],
        ['[data-site-status-commit]', element()],
        ['[data-site-freshness]', element()],
        ['[data-site-content]', element()],
        ['[data-site-readiness]', element()],
        ['[data-site-docs]', element()],
        ['[data-site-sources]', element()],
        ['[data-site-search]', element()],
    ]);
    const claims = [element({ 'data-site-claim': 'metrics.docs.authored' })];
    return {
        ownerDocument: {
            createElement() { return element(); },
        },
        querySelector(selector) { return nodes.get(selector) ?? null; },
        querySelectorAll(selector) { return selector === '[data-site-claim]' ? claims : []; },
        nodes,
        claims,
    };
}

function view()
{
    const search = new TextEncoder().encode(JSON.stringify({
        records: [{ slug: 'guide', title: 'Search result', sourcePath: 'wiki/Guide.md' }],
    }));
    return {
        state: 'current',
        banner: 'Current repository data',
        commit: 'a'.repeat(40),
        statusCommit: 'b'.repeat(40),
        bundle: {
            source: { commit: 'a'.repeat(40) },
            site: { home: { hero: { lede: 'Bundle-backed introduction' } } },
            globalRelease: { summary: 'Bundle-backed readiness' },
            metrics: [{ id: 'docs.authored', value: 42 }],
            docs: { documents: [{
                slug: 'guide', title: 'Guide', sourcePath: 'wiki/Guide.md',
                sourceUrl: `https://github.com/Krilliac/SparkEngine/blob/${'a'.repeat(40)}/wiki/Guide.md`,
            }] },
        },
        files: { docsSearch: search },
    };
}

describe('browser runtime entrypoint', () =>
{
    test('renders bundle-backed claims, docs, search, source links, SHA, and freshness banner', () =>
    {
        const page = root();
        renderSiteDataView(page, view());
        assert.equal(page.nodes.get('[data-site-banner]').textContent, 'Current repository data');
        assert.equal(page.nodes.get('[data-site-commit]').textContent, 'a'.repeat(40));
        assert.equal(page.nodes.get('[data-site-status-commit]').textContent, 'b'.repeat(40));
        assert.equal(page.nodes.get('[data-site-content]').textContent, 'Bundle-backed introduction');
        assert.equal(page.nodes.get('[data-site-readiness]').textContent, 'Bundle-backed readiness');
        assert.equal(page.claims[0].textContent, '42');
        assert.equal(page.nodes.get('[data-site-docs]').children[0].textContent, 'Guide');
        assert.equal(page.nodes.get('[data-site-docs]').children[0].href, '/docs/guide');
        assert.equal(page.nodes.get('[data-site-sources]').children[0].href,
                     `https://github.com/Krilliac/SparkEngine/blob/${'a'.repeat(40)}/wiki/Guide.md`);
        assert.equal(page.nodes.get('[data-site-search]').children[0].textContent, 'Search result');
        assert.equal(page.nodes.get('[data-site-search]').children[0].href, '/docs/guide');
        renderSiteDataView(page, {
            state: 'unavailable',
            banner: 'Repository data unavailable',
            commit: null,
            statusCommit: null,
            bundle: null,
            files: {},
        });
        assert.equal(page.nodes.get('[data-site-content]').textContent, '');
        assert.equal(page.nodes.get('[data-site-status-commit]').textContent, 'unavailable');
        assert.equal(page.nodes.get('[data-site-docs]').children.length, 0);
        assert.equal(page.nodes.get('[data-site-search]').children.length, 0);
        assert.equal(page.claims[0].textContent, '');
    });

    test('mounts the live runtime and releases its subscription and poller', () =>
    {
        const page = root();
        let listener;
        let started = 0;
        let stopped = 0;
        const runtime = {
            snapshot: () => view(),
            subscribe(callback) { listener = callback; return () => { listener = undefined; }; },
            startPolling() { started += 1; },
            stopPolling() { stopped += 1; },
        };
        const mounted = mountSiteDataRuntime({ runtime, root: page });
        assert.equal(started, 1);
        listener({ ...view(), state: 'stale', banner: 'Stale repository data' });
        assert.equal(page.nodes.get('[data-site-banner]').textContent, 'Stale repository data');
        mounted.unmount();
        assert.equal(stopped, 1);
    });
});
