import assert from 'node:assert/strict';
import { test } from 'node:test';

import { startSiteDataRuntime } from '../bootstrap.mjs';

test('bootstrap resolves a relative publication base against the page URL', async () =>
{
    const requested = [];
    const root = {
        baseURI: 'https://site.example/app/index.html',
        documentElement: { dataset: { siteDataBase: './site-data/' } },
        querySelector() { return null; },
        querySelectorAll() { return []; },
        defaultView: { addEventListener() {} },
    };
    const started = startSiteDataRuntime({
        root,
        displayedCommit: 'a'.repeat(40),
        fetch: async (url) =>
        {
            requested.push(url.href);
            return new Response(null, { status: 503 });
        },
    });
    try
    {
        await started.runtime.refresh();
        assert.equal(requested[0], 'https://site.example/app/site-data/status.json');
        assert.equal(started.runtime.snapshot().state, 'unavailable');
    }
    finally
    {
        started.unmount();
    }
});
