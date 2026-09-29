# Site-data runtime consumer

`siteDataRuntime.mjs` is the repository runtime's display boundary for the
generated site-data publication. Construct it with the `createFetchLoader`
export from `verifyBundle.mjs`. Display the verified commit from the runtime;
an optional expected-SHA pin is for fixed snapshots, not the moving site:

```js
import { createFetchLoader } from './verifyBundle.mjs';
import { mountSiteDataRuntime } from './browserRuntime.mjs';
import { SiteDataRuntime } from './siteDataRuntime.mjs';

const runtime = new SiteDataRuntime({
    load: createFetchLoader(new URL('/site-data/', document.baseURI)),
});
const mounted = mountSiteDataRuntime({ runtime });
```

The existing page supplies `[data-site-banner]`, `[data-site-commit]`,
`[data-site-freshness]`, `[data-site-content]`, `[data-site-readiness]`,
`[data-site-docs]`, `[data-site-search]`, and optional
`[data-site-claim="metrics.code.totalLines"]` targets. `unmount()` stops polling and
removes the subscription.

`index.html` calls `bootstrap.mjs` to mount these targets and start polling.
The host supplies the publication directory and its `/docs/<slug>` router.
This repository entry point does not deploy or alter the owner-managed site.

The runtime verifies `latest.json`, the bundle, exact-CI evidence, and the
pointed files before installing a publication. A failed or rejected refresh
retains the last verified publication, while `snapshot().state` and
`snapshot().banner` identify `syncing`, `blocked`, `stale`, or `unavailable`.
The fallback is therefore never presented as current. `stopPolling()` releases
the timer when the page is disposed.

The consumer also checks document/search membership, safe slugs, and exact-commit
source URLs before installing a snapshot. Markdown page links and anchors are
validated by the publisher. Generated-publication tests exercise the full
producer output; in-memory unit fixtures separately test lifecycle and rejection.

The verifier and consumer tests use the same generated publication selected by
`SPARK_SITE_DATA_DIR`. Run them with:

```powershell
$env:SPARK_SITE_DATA_DIR = '.site-data'; npm test --prefix tools/site-data/runtime
```
