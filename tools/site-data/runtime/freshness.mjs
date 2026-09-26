// Freshness classifier for the SparkEngine site-data runtime.
//
// The site caches a verified publication for MAX_AGE_SECONDS and may keep
// showing it for STALE_WHILE_REVALIDATE_SECONDS more while it re-fetches and
// re-verifies latest.json. Every rendered page carries one of five labels so an
// old or failed fallback is never presented as current:
//
//   current     verified, publication.state "current", within max-age
//   syncing     verified and "current", past max-age, revalidation pending,
//               still inside the stale-while-revalidate window
//   blocked     verified, but the producer published state "blocked" (the
//               same-commit CI conclusion was not success, or the tree was dirty)
//   stale       the shown data is past max-age and revalidation failed or was
//               rejected, or it is past max-age + stale-while-revalidate
//   unavailable nothing verified is available to show
//
// Staleness outranks "blocked": an outdated copy cannot vouch for the current
// state of the source branch, blocked or not.

export const MAX_AGE_SECONDS = 300;
export const STALE_WHILE_REVALIDATE_SECONDS = 300;

export const FRESHNESS_STATES = Object.freeze(['current', 'syncing', 'blocked', 'stale', 'unavailable']);

/**
 * Outcome of the most recent attempt to refresh the cached publication:
 *  - "verified":     the fetch succeeded and verifyPublishedBundle() accepted it
 *  - "revalidating": a fetch is in flight
 *  - "failed":       the fetch failed (network, HTTP status, byte cap)
 *  - "rejected":     the fetch succeeded but verifyPublishedBundle() refused it
 */
export const FETCH_OUTCOMES = Object.freeze(['verified', 'revalidating', 'failed', 'rejected']);

/**
 * Classify what the site is about to display.
 *
 * @param {object} input
 * @param {"current"|"blocked"|null} input.publicationState publication.state of
 *        the last verified bundle the site holds, or null when it holds none
 * @param {string} input.fetchOutcome one of FETCH_OUTCOMES
 * @param {number|null} input.verifiedAt epoch milliseconds when the held bundle
 *        was fetched and verified, or null when none is held
 * @param {number} input.now epoch milliseconds
 * @param {number} [input.maxAgeSeconds] freshness lifetime (default 300)
 * @param {number} [input.staleWhileRevalidateSeconds] grace window (default 300)
 * @returns {{state: string, ageSeconds: number|null, revalidate: boolean, reason: string}}
 *          `revalidate` tells the runtime to start a background fetch
 * @throws {RangeError} on inputs outside the documented domain
 */
export function classifyFreshness({
    publicationState,
    fetchOutcome,
    verifiedAt,
    now,
    maxAgeSeconds = MAX_AGE_SECONDS,
    staleWhileRevalidateSeconds = STALE_WHILE_REVALIDATE_SECONDS,
})
{
    if (!FETCH_OUTCOMES.includes(fetchOutcome))
    {
        throw new RangeError(`fetchOutcome must be one of ${FETCH_OUTCOMES}, got ${JSON.stringify(fetchOutcome)}`);
    }
    if (!Number.isFinite(now))
    {
        throw new RangeError('now must be a finite epoch-millisecond value');
    }
    for (const [name, value] of [['maxAgeSeconds', maxAgeSeconds],
                                 ['staleWhileRevalidateSeconds', staleWhileRevalidateSeconds]])
    {
        if (!Number.isFinite(value) || value < 0)
        {
            throw new RangeError(`${name} must be a finite non-negative number`);
        }
    }

    if (publicationState === null || publicationState === undefined)
    {
        return {
            state: 'unavailable',
            ageSeconds: null,
            revalidate: fetchOutcome !== 'revalidating',
            reason: 'no verified publication is held',
        };
    }
    if (publicationState !== 'current' && publicationState !== 'blocked')
    {
        throw new RangeError(`publicationState must be "current", "blocked", or null, got ${JSON.stringify(publicationState)}`);
    }
    if (!Number.isFinite(verifiedAt))
    {
        throw new RangeError('verifiedAt must be a finite epoch-millisecond value when a publication is held');
    }

    // A clock that moved backwards must not make old data look fresher than
    // "just verified"; clamp to zero rather than trusting a negative age.
    const ageSeconds = Math.max(0, (now - verifiedAt) / 1000);
    const expired = ageSeconds > maxAgeSeconds;
    const revalidate = expired && fetchOutcome !== 'revalidating';

    if (ageSeconds > maxAgeSeconds + staleWhileRevalidateSeconds)
    {
        return { state: 'stale', ageSeconds, revalidate, reason: 'past max-age and the stale-while-revalidate window' };
    }
    if (expired && (fetchOutcome === 'failed' || fetchOutcome === 'rejected'))
    {
        return { state: 'stale', ageSeconds, revalidate, reason: `revalidation ${fetchOutcome} after max-age` };
    }
    if (publicationState === 'blocked')
    {
        return { state: 'blocked', ageSeconds, revalidate, reason: 'the producer published a blocked state' };
    }
    if (expired)
    {
        return { state: 'syncing', ageSeconds, revalidate, reason: 'past max-age, revalidating' };
    }
    return { state: 'current', ageSeconds, revalidate, reason: 'verified within max-age' };
}
