import { createFetchLoader } from './verifyBundle.mjs';
import { mountSiteDataRuntime } from './browserRuntime.mjs';
import { SiteDataRuntime } from './siteDataRuntime.mjs';

/** Start the repository site runtime and release it when the page is hidden. */
export function startSiteDataRuntime({ root = globalThis.document, baseUrl, displayedCommit, fetch: fetchImpl } = {})
{
    const documentRoot = root?.documentElement;
    const publicationBase = new URL(baseUrl ?? documentRoot?.dataset.siteDataBase ?? '/site-data/', root?.baseURI);
    const commit = displayedCommit ?? documentRoot?.dataset.siteCommit;
    const runtime = new SiteDataRuntime({
        load: createFetchLoader(publicationBase, { fetch: fetchImpl }),
        displayedCommit: commit || undefined,
    });
    const mounted = mountSiteDataRuntime({ runtime, root });
    const pagehide = () => mounted.unmount();
    root.defaultView?.addEventListener?.('pagehide', pagehide, { once: true });
    return { runtime, unmount: pagehide };
}
