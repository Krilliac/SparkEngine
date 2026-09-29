// Browser entrypoint for the repository site-data runtime.

import { parseStrictJson } from './verifyBundle.mjs';

/**
 * Mount verified bundle data into the existing site DOM.
 *
 * The site supplies elements with data-site-* attributes. Values are assigned
 * with textContent or DOM nodes; no bundle field is interpreted as HTML. The
 * runtime remains the sole source for mutable claims, while the banner and
 * displayed commit make fallback state visible to visitors.
 *
 * @param {{runtime: import('./siteDataRuntime.mjs').SiteDataRuntime,
 *          root?: Document|Element, startPolling?: boolean}} options
 * @returns {{unmount: () => void, render: (object) => void}}
 */
export function mountSiteDataRuntime({ runtime, root = globalThis.document, startPolling = true })
{
    if (!runtime || typeof runtime.snapshot !== 'function' || typeof runtime.subscribe !== 'function')
    {
        throw new TypeError('runtime must be a SiteDataRuntime');
    }
    if (!root || typeof root.querySelector !== 'function')
    {
        throw new TypeError('root must be a Document or Element');
    }

    const render = (view) => renderSiteDataView(root, view);
    const unsubscribe = runtime.subscribe(render);
    render(runtime.snapshot());
    if (startPolling)
    {
        runtime.startPolling();
    }
    return {
        render,
        unmount: () =>
        {
            unsubscribe();
            runtime.stopPolling();
        },
    };
}

/** Render only bundle-backed text and links, with explicit fallback labels. */
export function renderSiteDataView(root, view)
{
    setText(root, '[data-site-banner]', view.banner);
    setText(root, '[data-site-commit]', view.commit ?? 'unavailable');
    setText(root, '[data-site-freshness]', view.state);

    const bundle = view.bundle;
    setText(root, '[data-site-content]', bundle?.site?.home?.hero?.lede ?? '');
    setText(root, '[data-site-readiness]', bundle?.globalRelease?.summary ?? '');
    for (const element of root.querySelectorAll?.('[data-site-claim]') ?? [])
    {
        const value = claimValue(bundle, element.getAttribute('data-site-claim'));
        element.textContent = value === undefined ? '' : typeof value === 'string' ? value : JSON.stringify(value);
    }
    renderCollection(root, '[data-site-docs]', bundle?.docs?.documents, (document) =>
    {
        const element = globalThis.document?.createElement?.('a') ?? root.ownerDocument?.createElement?.('a');
        if (!element)
        {
            return null;
        }
        element.textContent = String(document.title ?? document.slug ?? '');
        element.href = safeDocRoute(document.slug);
        element.dataset.sourceUrl = String(document.sourceUrl ?? '');
        return element;
    });
    renderCollection(root, '[data-site-sources]', bundle?.docs?.documents, (document) =>
    {
        const element = globalThis.document?.createElement?.('a') ?? root.ownerDocument?.createElement?.('a');
        if (!element)
        {
            return null;
        }
        element.textContent = `Source: ${String(document.title ?? document.slug ?? '')}`;
        element.href = safeSourceUrl(document, bundle);
        return element;
    });
    let searchRecords;
    const searchBytes = view.files?.docsSearch;
    if (searchBytes instanceof Uint8Array)
    {
        try
        {
            searchRecords = parseStrictJson(searchBytes, 'verified docs search JSON', 16 * 1024 * 1024).records;
        }
        catch
        {
            searchRecords = undefined;
        }
    }
    renderCollection(root, '[data-site-search]', searchRecords, (record) =>
    {
        const element = globalThis.document?.createElement?.('a') ?? root.ownerDocument?.createElement?.('a');
        if (!element)
        {
            return null;
        }
        element.textContent = String(record.title ?? record.slug ?? '');
        element.href = safeDocRoute(record.slug);
        return element;
    });
}

function claimValue(bundle, path)
{
    if (typeof path !== 'string' || path.length === 0)
    {
        return undefined;
    }
    if (path.startsWith('metrics.'))
    {
        const metric = Array.isArray(bundle?.metrics)
            ? bundle.metrics.find((entry) => entry?.id === path.slice('metrics.'.length)) : undefined;
        return metric?.value;
    }
    let value = bundle;
    for (const part of path.split('.'))
    {
        if (value === null || value === undefined || typeof value !== 'object' || !(part in value))
        {
            return undefined;
        }
        value = value[part];
    }
    return value;
}

function safeDocRoute(slug)
{
    return typeof slug === 'string' && /^[a-z0-9][a-z0-9/_-]*$/.test(slug) && !slug.includes('//')
        ? `/docs/${slug}` : '#';
}

function safeSourceUrl(document, bundle)
{
    const commit = bundle?.source?.commit;
    const sourcePath = document?.sourcePath;
    const expected = typeof commit === 'string' && typeof sourcePath === 'string'
        ? `https://github.com/Krilliac/SparkEngine/blob/${commit}/${sourcePath}`
        : null;
    return expected !== null && document.sourceUrl === expected ? expected : '#';
}

function setText(root, selector, value)
{
    const element = root.querySelector(selector);
    if (element)
    {
        element.textContent = String(value);
    }
}

function renderCollection(root, selector, values, create)
{
    const container = root.querySelector(selector);
    if (!container)
    {
        return;
    }
    while (container.firstChild)
    {
        container.removeChild(container.firstChild);
    }
    for (const value of Array.isArray(values) ? values : [])
    {
        const element = create(value);
        if (element)
        {
            container.appendChild(element);
        }
    }
}
