// Reference runtime verifier for the SparkEngine site-data publication.
//
// The site runtime imports this module to decide whether a fetched site-data
// publication (the moving `site-data` tag produced by tools/site-data/generate.py)
// may be displayed. It is the consumer-side mirror of
// validate_published_bundle() in tools/site-data/validate.py: the same budgets,
// the same schema version, the same pointer, hash, and identity rules. The
// node:test suite in test/ runs both validators over the same generated and
// tampered bundles, so a rule that drifts on either side fails the
// site-runtime-contract CI job.
//
// Dependency-free by design: it uses only WebCrypto, TextDecoder/TextEncoder,
// URL, and fetch, so the same file runs in a browser, a worker, or Node 20+.
//
// The runtime is deliberately stricter than the producer-side validator in a
// few places that only matter when the bytes arrive over HTTP: pointer paths are
// limited to a URL-safe character set, publication.state must be one of the two
// states the producer emits, and source.commit must be a full hexadecimal object
// id that equals bundleVersion. None of these can reject a bundle that
// generate.py produces.
//
// One producer rule is deliberately not repeated here: the full schema of the
// exact CI evidence manifest (exact_evidence.py validate_manifest, reached from
// validate_published_bundle). Re-deriving that manifest needs the publisher's
// workflow inputs, which the site never sees. The runtime still binds the
// manifest to the displayed commit (sourceCommit) and requires the hash-checked
// exactCiEvidence file to equal publication.exactEvidence, so the evidence
// shown is byte-for-byte the manifest latest.json points to. (Like every check
// here, this is integrity, not authenticity: latest.json itself is unsigned.)

// Publication contract constants. Keep these equal to tools/site-data/common.py
// (SCHEMA_VERSION, MAX_JSON_*) and to the budgets in generate.py
// enforce_publication_budgets() / validate.py validate_published_bundle().
// test/verifyBundle.test.mjs asserts the common.py values directly and proves
// the budgets behaviourally against the Python validator.
export const SCHEMA_VERSION = 1;
export const LATEST_MAX_BYTES = 32 * 1024;
export const BUNDLE_MAX_BYTES = 5 * 1024 * 1024;
export const DOCUMENT_PAGE_MAX_BYTES = 2 * 1024 * 1024;
export const EXACT_EVIDENCE_MAX_BYTES = 32 * 1024;
export const DEFAULT_FILE_MAX_BYTES = 16 * 1024 * 1024;
export const MAX_JSON_NODES = 250_000;
export const MAX_JSON_DEPTH = 128;
export const MAX_JSON_STRING_BYTES = 2 * 1024 * 1024;

export const PUBLICATION_STATES = Object.freeze(['current', 'blocked']);

const COMMIT_PATTERN = /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/;
const SHA256_PATTERN = /^[0-9a-f]{64}$/;
const PATH_SEGMENT_PATTERN = /^[A-Za-z0-9._~-]+$/;

/** Raised when a publication fails verification; `errors` lists every finding. */
export class BundleVerificationError extends Error
{
    constructor(errors)
    {
        const list = Array.isArray(errors) ? errors : [String(errors)];
        const detail = list.map((message) => `  - ${message}`).join('\n');
        super(`published bundle validation failed with ${list.length} error(s):\n${detail}`);
        this.name = 'BundleVerificationError';
        this.errors = list;
    }
}

/**
 * Reject pointer paths that could leave the publication root or change meaning
 * when resolved as a URL. Mirrors validate.py (absolute, `..`, backslash) and
 * adds URL-specific refusals (schemes, queries, fragments, percent escapes,
 * empty or `.` segments).
 *
 * @param {unknown} path candidate relative path from a pointer
 * @returns {string|null} a reason the path is unsafe, or null when it is safe
 */
export function unsafePathReason(path)
{
    if (typeof path !== 'string' || path.length === 0)
    {
        return 'path is not a non-empty string';
    }
    if (path.startsWith('/'))
    {
        return 'path is absolute';
    }
    if (path.includes('\\'))
    {
        return 'path contains a backslash';
    }
    for (const segment of path.split('/'))
    {
        if (segment === '..')
        {
            return 'path contains a parent-directory segment';
        }
        if (segment === '' || segment === '.')
        {
            return 'path contains an empty or current-directory segment';
        }
        if (!PATH_SEGMENT_PATTERN.test(segment))
        {
            return 'path contains characters outside the URL-safe set';
        }
    }
    return null;
}

function utf8ByteLength(text)
{
    return new TextEncoder().encode(text).length;
}

function isWellFormedString(text)
{
    if (typeof text.isWellFormed === 'function')
    {
        return text.isWellFormed();
    }
    return !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(text);
}

// JSON.parse keeps the last of two duplicate keys. The producer refuses
// duplicates (common.py _reject_duplicate_keys) because a reviewer sees the first
// value while a consumer uses the last, so the runtime refuses them too. The text
// has already been accepted by JSON.parse, so this scan only tracks structure.
function rejectDuplicateKeys(text, label)
{
    const stack = [];
    let index = 0;
    while (index < text.length)
    {
        const character = text[index];
        if (character === '"')
        {
            let end = index + 1;
            while (text[end] !== '"')
            {
                end += text[end] === '\\' ? 2 : 1;
            }
            const top = stack[stack.length - 1];
            if (top !== undefined && top.keys !== null && top.expectKey)
            {
                const key = JSON.parse(text.slice(index, end + 1));
                if (top.keys.has(key))
                {
                    throw new Error(`Invalid JSON in ${label}: duplicate object key ${JSON.stringify(key)}`);
                }
                top.keys.add(key);
                top.expectKey = false;
            }
            index = end + 1;
            continue;
        }
        if (character === '{')
        {
            stack.push({ keys: new Set(), expectKey: true });
        }
        else if (character === '[')
        {
            stack.push({ keys: null, expectKey: false });
        }
        else if (character === '}' || character === ']')
        {
            stack.pop();
        }
        else if (character === ',')
        {
            const top = stack[stack.length - 1];
            if (top !== undefined && top.keys !== null)
            {
                top.expectKey = true;
            }
        }
        index += 1;
    }
}

/**
 * Decode JSON bytes with the producer's rules (common.py decode_json_bytes):
 * strict UTF-8 without a BOM, no duplicate keys, no non-finite numbers, and
 * bounded size, node count, depth, and string length.
 *
 * @param {Uint8Array} bytes raw payload
 * @param {string} label name used in error messages
 * @param {number} maximum byte bound for the payload
 * @returns {unknown} the decoded JSON value
 */
export function parseStrictJson(bytes, label, maximum)
{
    if (bytes.length > maximum)
    {
        throw new Error(`${label} exceeds the ${maximum}-byte JSON bound`);
    }
    let text;
    try
    {
        text = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes);
    }
    catch (error)
    {
        throw new Error(`Invalid UTF-8 in ${label}: ${error.message}`);
    }
    let decoded;
    try
    {
        decoded = JSON.parse(text);
    }
    catch (error)
    {
        throw new Error(`Invalid JSON in ${label}: ${error.message}`);
    }
    rejectDuplicateKeys(text, label);

    let nodes = 0;
    const stack = [[decoded, 1]];
    while (stack.length > 0)
    {
        const [item, depth] = stack.pop();
        nodes += 1;
        if (nodes > MAX_JSON_NODES)
        {
            throw new Error(`${label} exceeds the ${MAX_JSON_NODES}-node JSON bound`);
        }
        if (depth > MAX_JSON_DEPTH)
        {
            throw new Error(`${label} exceeds the ${MAX_JSON_DEPTH}-level JSON depth bound`);
        }
        if (Array.isArray(item))
        {
            for (const child of item)
            {
                stack.push([child, depth + 1]);
            }
        }
        else if (item !== null && typeof item === 'object')
        {
            for (const [key, child] of Object.entries(item))
            {
                checkString(key, label, 'object key');
                stack.push([child, depth + 1]);
            }
        }
        else if (typeof item === 'string')
        {
            checkString(item, label, 'string');
        }
        else if (typeof item === 'number' && !Number.isFinite(item))
        {
            throw new Error(`${label} contains a non-finite JSON number`);
        }
    }
    return decoded;
}

function checkString(text, label, kind)
{
    if (!isWellFormedString(text))
    {
        throw new Error(`${label} contains invalid Unicode in a JSON ${kind}`);
    }
    // A UTF-16 code unit encodes to at most three UTF-8 bytes, so only strings
    // that could exceed the bound need an exact byte count.
    if (text.length * 3 > MAX_JSON_STRING_BYTES && utf8ByteLength(text) > MAX_JSON_STRING_BYTES)
    {
        const noun = kind === 'object key' ? 'JSON object key' : 'JSON string';
        throw new Error(`${label} contains an oversized ${noun}`);
    }
}

async function sha256Hex(bytes)
{
    const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes);
    return Array.from(new Uint8Array(digest), (value) => value.toString(16).padStart(2, '0')).join('');
}

function isObject(value)
{
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function jsonEqual(left, right)
{
    if (left === right)
    {
        return true;
    }
    if (Array.isArray(left))
    {
        return Array.isArray(right) && left.length === right.length &&
               left.every((value, index) => jsonEqual(value, right[index]));
    }
    if (isObject(left) && isObject(right))
    {
        const leftKeys = Object.keys(left);
        return leftKeys.length === Object.keys(right).length &&
               leftKeys.every((key) => Object.hasOwn(right, key) && jsonEqual(left[key], right[key]));
    }
    return false;
}

function objectField(container, key)
{
    const value = isObject(container) ? container[key] : undefined;
    return isObject(value) ? value : {};
}

function fileStem(path)
{
    const name = path.slice(path.lastIndexOf('/') + 1);
    const dot = name.lastIndexOf('.');
    return dot > 0 ? name.slice(0, dot) : name;
}

/**
 * Build a loader that fetches publication files below `baseUrl` with a hard
 * byte cap enforced while the body streams (Content-Length alone is not trusted:
 * a compressed response can decode past it).
 *
 * @param {string|URL} baseUrl directory URL that holds latest.json
 * @param {{fetch?: typeof fetch, init?: RequestInit}} [options] fetch
 *        implementation and request options (for example a cache mode)
 * @returns {(path: string, maximum: number) => Promise<Uint8Array>} loader
 */
export function createFetchLoader(baseUrl, { fetch: fetchImpl = globalThis.fetch, init = {} } = {})
{
    const base = new URL(baseUrl);
    if (!base.pathname.endsWith('/'))
    {
        base.pathname += '/';
    }
    base.search = '';
    base.hash = '';
    return async (path, maximum) =>
    {
        const reason = unsafePathReason(path);
        if (reason !== null)
        {
            throw new Error(`refusing to fetch ${JSON.stringify(path)}: ${reason}`);
        }
        const url = new URL(path, base);
        if (url.origin !== base.origin || !url.pathname.startsWith(base.pathname))
        {
            throw new Error(`refusing to fetch ${JSON.stringify(path)}: resolves outside the publication root`);
        }
        const response = await fetchImpl(url, init);
        if (!response.ok)
        {
            throw new Error(`HTTP ${response.status} for ${path}`);
        }
        const declared = Number(response.headers.get('content-length'));
        if (Number.isFinite(declared) && declared > maximum)
        {
            throw new Error(`${path} declares ${declared} bytes, over the ${maximum}-byte bound`);
        }
        if (response.body === null || typeof response.body.getReader !== 'function')
        {
            const whole = new Uint8Array(await response.arrayBuffer());
            if (whole.length > maximum)
            {
                throw new Error(`${path} exceeds the ${maximum}-byte bound`);
            }
            return whole;
        }
        const reader = response.body.getReader();
        const chunks = [];
        let total = 0;
        for (;;)
        {
            const { done, value } = await reader.read();
            if (done)
            {
                break;
            }
            total += value.length;
            if (total > maximum)
            {
                await reader.cancel();
                throw new Error(`${path} exceeds the ${maximum}-byte bound`);
            }
            chunks.push(value);
        }
        const payload = new Uint8Array(total);
        let offset = 0;
        for (const chunk of chunks)
        {
            payload.set(chunk, offset);
            offset += chunk.length;
        }
        return payload;
    };
}

async function readPointer(load, pointer, label, maximum, errors)
{
    if (!isObject(pointer))
    {
        errors.push(`${label} pointer is not an object`);
        return null;
    }
    const path = pointer.path;
    if (typeof path !== 'string')
    {
        errors.push(`${label} pointer has no path`);
        return null;
    }
    const reason = unsafePathReason(path);
    if (reason !== null)
    {
        errors.push(`${label} pointer path is unsafe: ${JSON.stringify(path)} (${reason})`);
        return null;
    }
    const limit = maximum ?? DEFAULT_FILE_MAX_BYTES;
    let payload;
    try
    {
        payload = await load(path, limit);
    }
    catch (error)
    {
        errors.push(`${label} cannot be read: ${error.message}`);
        return null;
    }
    if (!(payload instanceof Uint8Array))
    {
        errors.push(`${label} loader did not return bytes`);
        return null;
    }
    if (payload.length > limit)
    {
        errors.push(`${label} exceeds ${limit} bytes (${payload.length})`);
        return null;
    }
    if (pointer.bytes !== payload.length)
    {
        errors.push(`${label} byte count differs: pointer=${pointer.bytes} actual=${payload.length}`);
    }
    if (typeof pointer.sha256 !== 'string' || !SHA256_PATTERN.test(pointer.sha256) ||
        pointer.sha256 !== (await sha256Hex(payload)))
    {
        errors.push(`${label} SHA-256 differs`);
    }
    return payload;
}

function checkDocumentPage(page, slug, latest, sourceCommit, errors)
{
    if (!isObject(page))
    {
        errors.push(`document ${JSON.stringify(slug)} JSON is invalid: document JSON is not an object`);
        return;
    }
    if (page.sourceCommit !== sourceCommit || page.bundleVersion !== latest.bundleVersion)
    {
        errors.push(`document ${JSON.stringify(slug)} identity differs from latest`);
    }
    if (objectField(page, 'document').slug !== slug)
    {
        errors.push(`document payload slug differs for ${JSON.stringify(slug)}`);
    }
}

async function verifyDocument(load, document, filesBySlug, latest, sourceCommit, fetchPage, errors)
{
    const slug = isObject(document) ? document.slug : undefined;
    const pointer = isObject(document) ? document.published : undefined;
    if (!jsonEqual(pointer, filesBySlug[slug]))
    {
        errors.push(`document pointer differs from filesBySlug for ${JSON.stringify(slug)}`);
    }
    if (!isObject(pointer))
    {
        return null;
    }
    if (document.contentPath !== pointer.path || document.contentSha256 !== pointer.sha256 ||
        document.contentBytes !== pointer.bytes)
    {
        errors.push(`document compatibility pointer differs for ${JSON.stringify(slug)}`);
    }
    // Path safety is checked even when the page is not fetched now: a later
    // on-demand fetch must never be the first place an escaping path is noticed.
    const reason = unsafePathReason(pointer.path);
    if (reason !== null)
    {
        errors.push(`document ${slug} pointer path is unsafe: ${JSON.stringify(pointer.path)} (${reason})`);
        return null;
    }
    if (fileStem(pointer.path) !== pointer.sha256)
    {
        errors.push(`document ${JSON.stringify(slug)} is not hash-addressed`);
    }
    if (!fetchPage)
    {
        return null;
    }
    const bytes = await readPointer(load, pointer, `document ${slug}`, DOCUMENT_PAGE_MAX_BYTES, errors);
    if (bytes === null)
    {
        return null;
    }
    let page;
    try
    {
        page = parseStrictJson(bytes, `document ${JSON.stringify(slug)} JSON`, DOCUMENT_PAGE_MAX_BYTES);
    }
    catch (error)
    {
        errors.push(`document ${JSON.stringify(slug)} JSON is invalid: ${error.message}`);
        return null;
    }
    checkDocumentPage(page, slug, latest, sourceCommit, errors);
    return page;
}

function checkIdentity(latest, bundle, errors)
{
    const sourceCommit = objectField(latest, 'source').commit;
    if (typeof sourceCommit !== 'string' || !COMMIT_PATTERN.test(sourceCommit))
    {
        errors.push('latest.json source.commit is not a full hexadecimal commit id');
    }
    if (latest.bundleVersion !== sourceCommit)
    {
        errors.push('latest.json bundleVersion differs from source.commit');
    }
    if (bundle.bundleVersion !== latest.bundleVersion)
    {
        errors.push('bundleVersion differs between latest and bundle');
    }
    if (objectField(bundle, 'source').commit !== sourceCommit)
    {
        errors.push('source commit differs between latest and bundle');
    }
    const publication = objectField(latest, 'publication');
    if (!jsonEqual(bundle.publication, publication))
    {
        errors.push('publication metadata differs between latest and bundle');
    }
    if (publication.evidenceCommit !== sourceCommit)
    {
        errors.push('publication evidenceCommit differs from source commit');
    }
    if (!PUBLICATION_STATES.includes(publication.state))
    {
        errors.push(`publication state ${JSON.stringify(publication.state)} is not one of ${PUBLICATION_STATES}`);
    }
    if (publication.state === 'current' && publication.conclusion !== 'success')
    {
        errors.push('current publication does not have a successful conclusion');
    }
    const exactEvidence = publication.exactEvidence;
    if (exactEvidence !== undefined && exactEvidence !== null)
    {
        if (!isObject(exactEvidence))
        {
            errors.push('publication exactEvidence is not an object');
        }
        else if (exactEvidence.sourceCommit !== sourceCommit)
        {
            errors.push('publication exactEvidence sourceCommit differs from source commit');
        }
    }
    return sourceCommit;
}

async function verifyDocs(load, latest, bundle, files, sourceCommit, verifyDocuments, errors)
{
    const docs = objectField(bundle, 'docs');
    const documents = Array.isArray(docs.documents) ? docs.documents : [];
    const filesBySlug = objectField(docs, 'filesBySlug');
    const counts = new Map();
    for (const document of documents)
    {
        const slug = isObject(document) ? document.slug : undefined;
        counts.set(slug, (counts.get(slug) ?? 0) + 1);
    }
    for (const [slug, count] of counts)
    {
        if (!slug || count > 1)
        {
            errors.push(`invalid or duplicate document slug: ${JSON.stringify(slug ?? null)}`);
        }
    }
    for (const document of documents)
    {
        await verifyDocument(load, document, filesBySlug, latest, sourceCommit, verifyDocuments, errors);
    }

    const searchPointer = files.docsSearch;
    if (isObject(searchPointer) &&
        (docs.searchPath !== searchPointer.path || docs.searchSha256 !== searchPointer.sha256 ||
         docs.searchBytes !== searchPointer.bytes))
    {
        errors.push('bundle docs search pointer differs from latest');
    }
}

/**
 * Verify a published site-data bundle before the site displays any of it.
 *
 * Reads latest.json (32 KiB), the bundle (5 MiB), and every other file latest.json
 * points to, checking byte counts and SHA-256 with WebCrypto, pointer-path
 * safety, schemaVersion, and that latest, bundle, and publication metadata name
 * the same commit. Document pages (2 MiB each) are verified here when
 * `verifyDocuments` is set; otherwise the runtime verifies each one on demand
 * with verifyDocumentPage().
 *
 * @param {(path: string, maximum: number) => Promise<Uint8Array>} load reads a
 *        publication-relative path, refusing more than `maximum` bytes
 * @param {{displayedCommit?: string, verifyDocuments?: boolean}} [options]
 *        `displayedCommit` is the SHA the page is about to display; it must
 *        equal latest.source.commit
 * @returns {Promise<{latest: object, bundle: object, commit: string,
 *          publication: object, files: Object<string, Uint8Array>}>}
 * @throws {BundleVerificationError} listing every failed rule
 */
export async function verifyPublishedBundle(load, { displayedCommit, verifyDocuments = false } = {})
{
    let latest;
    try
    {
        latest = parseStrictJson(await load('latest.json', LATEST_MAX_BYTES), 'published latest.json',
                                 LATEST_MAX_BYTES);
    }
    catch (error)
    {
        throw new BundleVerificationError([`cannot read published latest.json: ${error.message}`]);
    }
    if (!isObject(latest))
    {
        throw new BundleVerificationError(['published latest.json must be a JSON object']);
    }

    const errors = [];
    if (latest.schemaVersion !== SCHEMA_VERSION)
    {
        errors.push('latest.json schemaVersion is unsupported');
    }
    if (!isObject(latest.files))
    {
        errors.push('latest.json files is not an object');
    }
    const files = objectField(latest, 'files');
    const contents = {};

    let bundle = {};
    const bundleBytes = await readPointer(load, files.bundle, 'bundle', BUNDLE_MAX_BYTES, errors);
    if (bundleBytes !== null)
    {
        try
        {
            bundle = parseStrictJson(bundleBytes, 'bundle JSON', BUNDLE_MAX_BYTES);
        }
        catch (error)
        {
            errors.push(`bundle JSON is invalid: ${error.message}`);
        }
        if (!isObject(bundle))
        {
            errors.push('bundle JSON is not an object');
            bundle = {};
        }
        contents.bundle = bundleBytes;
    }
    const sourceCommit = checkIdentity(latest, bundle, errors);
    if (bundleBytes !== null)
    {
        await verifyDocs(load, latest, bundle, files, sourceCommit, verifyDocuments, errors);
    }

    for (const [label, pointer] of Object.entries(files))
    {
        if (label === 'bundle')
        {
            continue;
        }
        const maximum = label === 'exactCiEvidence' ? EXACT_EVIDENCE_MAX_BYTES : DEFAULT_FILE_MAX_BYTES;
        const name = label === 'exactCiEvidence' ? 'exact CI evidence' : `latest file ${label}`;
        const payload = await readPointer(load, pointer, name, maximum, errors);
        if (payload === null)
        {
            continue;
        }
        contents[label] = payload;
        if (label === 'exactCiEvidence')
        {
            try
            {
                const evidence = parseStrictJson(payload, 'exact CI evidence JSON', EXACT_EVIDENCE_MAX_BYTES);
                if (!jsonEqual(evidence, objectField(latest, 'publication').exactEvidence))
                {
                    errors.push('exact CI evidence file differs from publication metadata');
                }
            }
            catch (error)
            {
                errors.push(`exact CI evidence JSON is invalid: ${error.message}`);
            }
        }
    }

    if (displayedCommit !== undefined && displayedCommit !== sourceCommit)
    {
        errors.push(`displayed commit ${JSON.stringify(displayedCommit)} differs from latest.source.commit`);
    }
    if (errors.length > 0)
    {
        throw new BundleVerificationError(errors);
    }
    return { latest, bundle, commit: sourceCommit, publication: objectField(latest, 'publication'), files: contents };
}

/**
 * Verify and decode one documentation page of an already verified publication.
 *
 * @param {(path: string, maximum: number) => Promise<Uint8Array>} load loader
 * @param {{latest: object, bundle: object, commit: string}} verified result of
 *        verifyPublishedBundle()
 * @param {string} slug document slug from bundle.docs.documents
 * @returns {Promise<object>} the decoded page payload
 * @throws {BundleVerificationError} when the page or its pointer fails a rule
 */
export async function verifyDocumentPage(load, verified, slug)
{
    const docs = objectField(verified.bundle, 'docs');
    const documents = Array.isArray(docs.documents) ? docs.documents : [];
    const document = documents.find((candidate) => isObject(candidate) && candidate.slug === slug);
    if (document === undefined)
    {
        throw new BundleVerificationError([`document ${JSON.stringify(slug)} is not in the bundle`]);
    }
    const errors = [];
    const page = await verifyDocument(load, document, objectField(docs, 'filesBySlug'), verified.latest,
                                      verified.commit, true, errors);
    if (errors.length > 0 || page === null)
    {
        throw new BundleVerificationError(errors.length > 0 ? errors : [`document ${slug} could not be read`]);
    }
    return page;
}
