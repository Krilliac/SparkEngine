// Stateful site-data consumer used by the repository site runtime.
//
// The verifier is intentionally stateless so it can also be used by build
// checks. This consumer owns the display boundary: only a verified publication
// is exposed, and a rejected refresh keeps the previous publication with an
// explicit stale/unavailable label.

import { BundleVerificationError, parseStrictJson, verifyPublishedBundle } from './verifyBundle.mjs';
import { classifyFreshness, MAX_AGE_SECONDS, STALE_WHILE_REVALIDATE_SECONDS } from './freshness.mjs';

const BANNER_TEXT = Object.freeze({
    current: 'Current repository data',
    syncing: 'Repository data is syncing',
    blocked: 'Publication blocked; repository data is not current',
    stale: 'Stale repository data; refresh failed or the freshness window expired',
    unavailable: 'Repository data unavailable; no verified publication is loaded',
});

// Hashes establish byte integrity; navigation still needs agreement between
// the independently stored document catalog and search index before display.
function verifyNavigation(verified)
{
    const { bundle, commit, files } = verified;
    const documents = bundle.docs?.documents;
    const search = parseStrictJson(files.docsSearch, 'docs search JSON', 16 * 1024 * 1024);
    const errors = [];
    if (bundle.schemaVersion !== 1 || search.schemaVersion !== 1 || search.sourceCommit !== commit ||
        !Array.isArray(documents) || !Array.isArray(search.records))
    {
        throw new BundleVerificationError(['documentation navigation schema or source commit is invalid']);
    }
    const bySlug = new Map();
    for (const document of documents)
    {
        const sourcePath = document?.sourcePath;
        const slug = document?.slug;
        if (typeof slug !== 'string' || !/^[a-z0-9][a-z0-9/_-]*$/.test(slug) || slug.includes('//') ||
            typeof sourcePath !== 'string' || /[\\?#%]/.test(sourcePath) ||
            sourcePath.split('/').some((part) => part === '' || part === '.' || part === '..') ||
            document.sourceUrl !== `https://github.com/Krilliac/SparkEngine/blob/${commit}/${sourcePath}`)
        {
            errors.push('document route or exact-commit source URL is invalid');
        }
        bySlug.set(slug, document);
    }
    const seen = new Set();
    for (const record of search.records)
    {
        const document = bySlug.get(record?.slug);
        if (!document || seen.has(record.slug) || record.sourcePath !== document.sourcePath)
        {
            errors.push('search record does not match a unique published document');
        }
        seen.add(record?.slug);
    }
    if (seen.size !== bySlug.size)
    {
        errors.push('search index does not cover every published document');
    }
    if (errors.length > 0)
    {
        throw new BundleVerificationError(errors);
    }
}

/**
 * Owns verified site-data state and its refresh lifecycle.
 *
 * The caller supplies a publication-relative loader (usually
 * createFetchLoader()). No network or storage implementation is hidden here.
 * The instance is safe to use from a browser event loop; refreshes are
 * serialized, and the last verified publication is retained as a fallback.
 */
export class SiteDataRuntime
{
    constructor({
        load,
        displayedCommit,
        now = () => Date.now(),
        maxAgeSeconds = MAX_AGE_SECONDS,
        staleWhileRevalidateSeconds = STALE_WHILE_REVALIDATE_SECONDS,
        onChange = () => {},
    })
    {
        if (typeof load !== 'function')
        {
            throw new TypeError('load must be a function');
        }
        if (typeof now !== 'function' || typeof onChange !== 'function')
        {
            throw new TypeError('now and onChange must be functions');
        }
        this.m_load = load;
        this.m_displayedCommit = displayedCommit;
        this.m_now = now;
        this.m_maxAgeSeconds = maxAgeSeconds;
        this.m_staleWhileRevalidateSeconds = staleWhileRevalidateSeconds;
        this.m_onChange = onChange;
        this.m_listeners = new Set();
        this.m_verified = null;
        this.m_verifiedAt = null;
        this.m_fetchOutcome = 'failed';
        this.m_error = null;
        this.m_refreshPromise = null;
        this.m_timer = null;
    }

    /** Fetch, verify, and atomically install a publication. */
    async refresh()
    {
        if (this.m_refreshPromise !== null)
        {
            return this.m_refreshPromise;
        }
        this.m_fetchOutcome = 'revalidating';
        this.m_refreshPromise = (async () =>
        {
            try
            {
                const verified = await verifyPublishedBundle(this.m_load, {
                    displayedCommit: this.m_displayedCommit,
                });
                verifyNavigation(verified);
                this.m_verified = verified;
                this.m_verifiedAt = this.m_now();
                this.m_fetchOutcome = 'verified';
                this.m_error = null;
            }
            catch (error)
            {
                this.m_fetchOutcome = error?.name === 'BundleVerificationError' ? 'rejected' : 'failed';
                this.m_error = error;
            }
            finally
            {
                this.m_refreshPromise = null;
                this.#notify();
            }
            return this.snapshot();
        })();
        // Publish the in-flight marker before notifying UI code. A render
        // callback may synchronously request a refresh; it must join this one.
        this.#notify();
        return this.m_refreshPromise;
    }

    /** Start periodic refreshes; the first refresh is immediate. */
    startPolling(intervalMs = (this.m_maxAgeSeconds * 1000))
    {
        if (!Number.isFinite(intervalMs) || intervalMs <= 0)
        {
            throw new RangeError('intervalMs must be a finite positive number');
        }
        this.stopPolling();
        void this.refresh();
        this.m_timer = setInterval(() => void this.refresh(), intervalMs);
        return this;
    }

    /** Stop periodic refreshes without discarding the verified fallback. */
    stopPolling()
    {
        if (this.m_timer !== null)
        {
            clearInterval(this.m_timer);
            this.m_timer = null;
        }
    }

    /** Subscribe to verified publication and freshness changes. */
    subscribe(listener)
    {
        if (typeof listener !== 'function')
        {
            throw new TypeError('listener must be a function');
        }
        this.m_listeners.add(listener);
        return () => this.m_listeners.delete(listener);
    }

    /** Return the display-safe state and verified content, if available. */
    snapshot(now = this.m_now())
    {
        const publicationState = this.m_verified?.publication?.state ?? null;
        const freshness = classifyFreshness({
            publicationState,
            fetchOutcome: this.m_fetchOutcome,
            verifiedAt: this.m_verifiedAt,
            now,
            maxAgeSeconds: this.m_maxAgeSeconds,
            staleWhileRevalidateSeconds: this.m_staleWhileRevalidateSeconds,
        });
        // The cache classifier describes age. The visible site additionally
        // discloses any failed refresh immediately, even within that lifetime.
        if (this.m_verified && ['failed', 'rejected'].includes(this.m_fetchOutcome))
        {
            freshness.state = 'stale';
            freshness.reason = `latest refresh ${this.m_fetchOutcome}; showing the last verified publication`;
            freshness.revalidate = true;
        }
        return {
            ...freshness,
            banner: BANNER_TEXT[freshness.state],
            commit: this.m_verified?.commit ?? null,
            bundle: this.m_verified?.bundle ?? null,
            publication: this.m_verified?.publication ?? null,
            files: this.m_verified?.files ?? {},
            error: this.m_error,
        };
    }

    #notify()
    {
        const view = this.snapshot();
        this.m_onChange(view);
        for (const listener of this.m_listeners)
        {
            listener(view);
        }
    }
}

export const FRESHNESS_BANNERS = BANNER_TEXT;
