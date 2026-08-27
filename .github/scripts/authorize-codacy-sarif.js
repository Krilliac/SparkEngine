// Trusted workflow_run authorization for Codacy SARIF upload.
//
// This file is executed only from a verified default-branch checkout. It never
// loads source-run code or downloaded artifact content.

const fs = require('fs');

const SOURCE_WORKFLOW_NAME = 'Codacy Security Scan';
const SOURCE_WORKFLOW_PATH = '.github/workflows/codacy.yml';
const REPORTER_WORKFLOW_PATH = '.github/workflows/codacy-report.yml';
const ARTIFACT_NAME = 'results.sarif';
const TRUSTED_PATHS = Object.freeze([
    REPORTER_WORKFLOW_PATH,
    '.github/scripts/authorize-codacy-sarif.js',
    '.github/scripts/test-authorize-codacy-sarif.js',
    '.github/scripts/validate-codacy-sarif.py',
    '.github/scripts/test_validate_codacy_sarif.py',
    '.github/scripts/test_workflow_privilege_boundaries.py'
]);
const SHA_PATTERN = /^[0-9a-f]{40}$/i;
const DIGEST_PATTERN = /^sha256:[0-9a-f]{64}$/i;
const MAX_ARTIFACT_BYTES = 100 * 1024 * 1024;
const MAX_LIST_ITEMS = 100;

const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const normalizeSha = value => SHA_PATTERN.test(value || '') ? value.toLowerCase() : null;
const workflowPath = value => typeof value === 'string' ? value.split('@')[0] : '';

function readEvent() {
    const eventPath = process.env.WORKFLOW_RUN_EVENT_PATH || process.env.GITHUB_EVENT_PATH;
    if (!eventPath) throw new Error('No workflow_run event path was provided.');
    const event = JSON.parse(fs.readFileSync(eventPath, 'utf8'));
    if (!isObject(event)) throw new Error('workflow_run event payload must be an object.');
    return event;
}

function repositoryMatches(value, owner, repo, expectedId = null) {
    return isObject(value) && value.full_name === `${owner}/${repo}` &&
        Number.isInteger(value.id) && value.id > 0 &&
        (expectedId === null || value.id === expectedId);
}

function splitRepository(value) {
    const pieces = String(value || '').split('/');
    return pieces.length === 2 && pieces.every(Boolean) ? pieces : null;
}

async function listOneBounded(method, request, field, label) {
    const response = await method({ ...request, per_page: MAX_LIST_ITEMS, page: 1 });
    const items = field ? response?.data?.[field] : response?.data;
    if (!Array.isArray(items)) throw new Error(`${label} response is not an array.`);
    if (items.length > MAX_LIST_ITEMS || String(response?.headers?.link || '').includes('rel="next"')) {
        throw new Error(`${label} exceeds the ${MAX_LIST_ITEMS}-item authorization limit.`);
    }
    return items;
}

async function attestFile(github, owner, repo, path, trustedSha, defaultBranch, errors) {
    try {
        const [checkoutFile, defaultFile] = await Promise.all([
            github.rest.repos.getContent({ owner, repo, path, ref: trustedSha }),
            github.rest.repos.getContent({ owner, repo, path, ref: defaultBranch })
        ]);
        if (!isObject(checkoutFile.data) || !isObject(defaultFile.data) ||
            checkoutFile.data.type !== 'file' || defaultFile.data.type !== 'file' ||
            !SHA_PATTERN.test(checkoutFile.data.sha || '') ||
            checkoutFile.data.sha.toLowerCase() !== String(defaultFile.data.sha || '').toLowerCase()) {
            errors.push(`Trusted reporter file '${path}' does not match the default branch.`);
        }
    } catch (error) {
        errors.push(`Could not attest trusted reporter file '${path}': ${error.message}`);
    }
}

function exactRunMatches(left, right) {
    return isObject(left) && isObject(right) &&
        left.id === right.id && left.workflow_id === right.workflow_id &&
        left.run_number === right.run_number && left.run_attempt === right.run_attempt &&
        left.name === right.name && workflowPath(left.path) === workflowPath(right.path) &&
        left.event === right.event && left.status === right.status &&
        left.conclusion === right.conclusion && left.head_branch === right.head_branch &&
        normalizeSha(left.head_sha) === normalizeSha(right.head_sha) &&
        left.repository?.id === right.repository?.id &&
        left.repository?.full_name === right.repository?.full_name &&
        left.head_repository?.id === right.head_repository?.id &&
        left.head_repository?.full_name === right.head_repository?.full_name;
}

function pullMatches(pull, repository, run) {
    return isObject(pull) && Number.isInteger(pull.number) && pull.number > 0 &&
        pull.state === 'open' && pull.base?.repo?.id === repository.id &&
        pull.base?.repo?.full_name === repository.full_name &&
        pull.base?.ref === repository.default_branch &&
        pull.head?.repo?.id === run.head_repository?.id &&
        pull.head?.repo?.full_name === run.head_repository?.full_name &&
        pull.head?.ref === run.head_branch && normalizeSha(pull.head?.sha) === normalizeSha(run.head_sha) &&
        normalizeSha(run.head_sha) !== null;
}

async function resolvePull(github, owner, repo, repository, eventRun, run, errors) {
    const references = [...(Array.isArray(eventRun.pull_requests) ? eventRun.pull_requests : []),
        ...(Array.isArray(run.pull_requests) ? run.pull_requests : [])];
    const numbers = [...new Set(references.map(value => value?.number)
        .filter(value => Number.isInteger(value) && value > 0))];

    if (numbers.length === 1) {
        try {
            const pull = (await github.rest.pulls.get({ owner, repo, pull_number: numbers[0] })).data;
            if (pullMatches(pull, repository, run)) return pull;
        } catch {
            // Fall through to the exact fork-owner/head-branch lookup.
        }
    }

    const source = splitRepository(run.head_repository?.full_name);
    if (!source || !run.head_branch) {
        errors.push('Source head repository and branch cannot identify a pull request.');
        return null;
    }
    try {
        const candidates = await listOneBounded(github.rest.pulls.list, {
            owner, repo, state: 'open', head: `${source[0]}:${run.head_branch}`
        }, null, 'pull-request candidate list');
        const exact = candidates.filter(pull => pullMatches(pull, repository, run));
        if (exact.length === 1 && (numbers.length === 0 ||
            (numbers.length === 1 && numbers[0] === exact[0].number))) return exact[0];
        errors.push(`Expected one exact source pull request, found ${exact.length}.`);
    } catch (error) {
        errors.push(`Could not resolve exact source pull request: ${error.message}`);
    }
    return null;
}

function artifactManifest(artifact) {
    return {
        id: artifact?.id ?? null,
        name: artifact?.name ?? null,
        size: artifact?.size_in_bytes ?? null,
        digest: artifact?.digest ?? null,
        workflowRun: isObject(artifact?.workflow_run) ? {
            id: artifact.workflow_run.id ?? null,
            repositoryId: artifact.workflow_run.repository_id ?? null,
            headRepositoryId: artifact.workflow_run.head_repository_id ?? null,
            headBranch: artifact.workflow_run.head_branch ?? null,
            headSha: artifact.workflow_run.head_sha ?? null
        } : null
    };
}

async function inspect(args) {
    const { github, context } = args;
    const { owner, repo } = context.repo;
    const errors = [];
    let event = null;
    let eventRun = null;
    let repository = null;
    let run = null;
    let pull = null;
    let artifact = null;
    let uploadRef = '';
    let uploadSha = '';

    try {
        event = readEvent();
        eventRun = event.workflow_run;
    } catch (error) {
        errors.push(`Could not read workflow_run event: ${error.message}`);
    }

    if (event?.action !== 'completed') errors.push('Expected a completed workflow_run event.');
    if (!repositoryMatches(event?.repository, owner, repo)) {
        errors.push('Event repository identity is invalid.');
    }
    if (!isObject(eventRun) || !Number.isInteger(eventRun.id) || eventRun.id < 1 ||
        !Number.isInteger(eventRun.workflow_id) || eventRun.workflow_id < 1 ||
        !Number.isInteger(eventRun.run_number) || eventRun.run_number < 1 ||
        !Number.isInteger(eventRun.run_attempt) || eventRun.run_attempt < 1 ||
        eventRun.name !== SOURCE_WORKFLOW_NAME || workflowPath(eventRun.path) !== SOURCE_WORKFLOW_PATH ||
        !['pull_request', 'push', 'schedule'].includes(eventRun.event) ||
        !SHA_PATTERN.test(eventRun.head_sha || '')) {
        errors.push('Event source workflow identity is invalid.');
    }

    try {
        repository = (await github.rest.repos.get({ owner, repo })).data;
        if (!repositoryMatches(repository, owner, repo, event?.repository?.id) ||
            typeof repository.default_branch !== 'string' || !repository.default_branch) {
            errors.push('API repository identity or default branch is invalid.');
        }
    } catch (error) {
        errors.push(`Could not load repository metadata: ${error.message}`);
    }

    const trustedSha = normalizeSha(process.env.TRUSTED_CHECKOUT_SHA);
    if (!trustedSha) errors.push('Trusted checkout SHA is invalid.');
    if (repository && trustedSha) {
        const expectedWorkflowRef = `${owner}/${repo}/${REPORTER_WORKFLOW_PATH}@refs/heads/${repository.default_branch}`;
        if (process.env.TRUSTED_WORKFLOW_REF !== expectedWorkflowRef ||
            normalizeSha(process.env.TRUSTED_WORKFLOW_SHA) !== trustedSha) {
            errors.push('Executing reporter workflow is not the exact trusted default-branch workflow.');
        }
        try {
            const defaultCommit = (await github.rest.repos.getCommit({
                owner, repo, ref: repository.default_branch
            })).data;
            if (normalizeSha(defaultCommit?.sha) !== trustedSha) {
                errors.push('Trusted checkout is not the exact current default-branch commit.');
            }
        } catch (error) {
            errors.push(`Could not attest default-branch commit: ${error.message}`);
        }
        await Promise.all(TRUSTED_PATHS.map(path => attestFile(
            github, owner, repo, path, trustedSha, repository.default_branch, errors
        )));
    }

    if (isObject(eventRun)) {
        try {
            run = (await github.rest.actions.getWorkflowRun({
                owner, repo, run_id: eventRun.id
            })).data;
            if (!exactRunMatches(run, eventRun)) {
                errors.push('API source workflow run does not exactly match the completed event.');
            }
        } catch (error) {
            errors.push(`Could not load exact source workflow run: ${error.message}`);
        }
    }

    if (run) {
        if (run.name !== SOURCE_WORKFLOW_NAME || workflowPath(run.path) !== SOURCE_WORKFLOW_PATH ||
            run.status !== 'completed' || run.conclusion !== 'success' ||
            !['pull_request', 'push', 'schedule'].includes(run.event) ||
            !repositoryMatches(run.repository, owner, repo, repository?.id) ||
            !splitRepository(run.head_repository?.full_name) || !normalizeSha(run.head_sha)) {
            errors.push('API source workflow metadata is not an exact successful Codacy scan.');
        }
    }

    if (run && repository && splitRepository(run.head_repository?.full_name)) {
        const [sourceOwner, sourceRepo] = splitRepository(run.head_repository.full_name);
        try {
            const [sourceFile, defaultFile] = await Promise.all([
                github.rest.repos.getContent({
                    owner: sourceOwner, repo: sourceRepo, path: SOURCE_WORKFLOW_PATH, ref: run.head_sha
                }),
                github.rest.repos.getContent({
                    owner, repo, path: SOURCE_WORKFLOW_PATH, ref: repository.default_branch
                })
            ]);
            if (!isObject(sourceFile.data) || !isObject(defaultFile.data) ||
                sourceFile.data.type !== 'file' || defaultFile.data.type !== 'file' ||
                !SHA_PATTERN.test(sourceFile.data.sha || '') ||
                sourceFile.data.sha.toLowerCase() !== String(defaultFile.data.sha || '').toLowerCase()) {
                errors.push('Source Codacy workflow blob does not match the trusted default branch.');
            }
        } catch (error) {
            errors.push(`Could not attest source Codacy workflow blob: ${error.message}`);
        }
    }

    if (run) {
        try {
            const runs = await listOneBounded(github.rest.actions.listWorkflowRuns, {
                owner, repo, workflow_id: run.workflow_id, event: run.event, head_sha: run.head_sha
            }, 'workflow_runs', 'same-head workflow-run list');
            const current = runs.find(candidate => candidate.id === run.id &&
                candidate.workflow_id === run.workflow_id && candidate.run_number === run.run_number &&
                candidate.run_attempt === run.run_attempt && normalizeSha(candidate.head_sha) === normalizeSha(run.head_sha));
            if (!current) errors.push('Exact source workflow run is absent from the same-head run inventory.');
            if (runs.some(candidate => candidate.workflow_id === run.workflow_id &&
                normalizeSha(candidate.head_sha) === normalizeSha(run.head_sha) &&
                (candidate.run_number > run.run_number ||
                    (candidate.run_number === run.run_number && candidate.run_attempt > run.run_attempt)))) {
                errors.push('A newer source workflow run or attempt exists for this exact head.');
            }
        } catch (error) {
            errors.push(`Could not attest latest source workflow run: ${error.message}`);
        }
    }

    if (run && repository) {
        if (run.event === 'pull_request') {
            pull = await resolvePull(github, owner, repo, repository, eventRun, run, errors);
            if (pull) {
                uploadRef = `refs/pull/${pull.number}/head`;
                uploadSha = normalizeSha(pull.head.sha) || '';
            }
        } else {
            if (run.head_repository?.id !== repository.id ||
                run.head_repository?.full_name !== repository.full_name ||
                run.head_branch !== repository.default_branch) {
                errors.push('Push/schedule source is not the exact default branch.');
            }
            uploadRef = `refs/heads/${repository.default_branch}`;
            uploadSha = normalizeSha(run.head_sha) || '';
        }
    }

    if (run && repository) {
        try {
            const artifacts = await listOneBounded(github.rest.actions.listWorkflowRunArtifacts, {
                owner, repo, run_id: run.id
            }, 'artifacts', 'source artifact inventory');
            if (artifacts.length !== 1 || artifacts[0]?.name !== ARTIFACT_NAME) {
                errors.push(`Expected exactly one '${ARTIFACT_NAME}' artifact.`);
            } else {
                artifact = artifacts[0];
                if (!Number.isInteger(artifact.id) || artifact.id < 1 || artifact.expired ||
                    !Number.isInteger(artifact.size_in_bytes) || artifact.size_in_bytes < 1 ||
                    artifact.size_in_bytes > MAX_ARTIFACT_BYTES ||
                    !DIGEST_PATTERN.test(artifact.digest || '')) {
                    errors.push(`Artifact '${ARTIFACT_NAME}' has invalid immutable metadata.`);
                }
                const provenance = artifact.workflow_run;
                if (!isObject(provenance) || provenance.id !== run.id ||
                    provenance.repository_id !== repository.id ||
                    provenance.head_repository_id !== run.head_repository?.id ||
                    provenance.head_branch !== run.head_branch ||
                    normalizeSha(provenance.head_sha) !== normalizeSha(run.head_sha)) {
                    errors.push(`Artifact '${ARTIFACT_NAME}' is not tied to the exact repository, run, and head.`);
                }
            }
        } catch (error) {
            errors.push(`Could not inspect source artifact: ${error.message}`);
        }
    }

    if (!uploadRef || !normalizeSha(uploadSha)) errors.push('Upload ref/SHA provenance is unresolved.');
    return {
        errors: [...new Set(errors)], event, eventRun, repository, run, pull, artifact,
        manifest: artifactManifest(artifact), uploadRef, uploadSha
    };
}

function setOutputs(core, inspection, authorized) {
    core.setOutput('authorized', authorized ? 'true' : 'false');
    core.setOutput('artifact-id', authorized ? String(inspection.manifest.id) : '');
    core.setOutput('artifact-digest', authorized ? inspection.manifest.digest : '');
    core.setOutput('artifact-manifest', authorized ? JSON.stringify(inspection.manifest) : '{}');
    core.setOutput('default-branch', authorized ? inspection.repository.default_branch : '');
    core.setOutput('pull-request-number', authorized ? String(inspection.pull?.number || '') : '');
    core.setOutput('source-event', authorized ? inspection.run.event : '');
    core.setOutput('source-sha', authorized ? normalizeSha(inspection.run.head_sha) : '');
    core.setOutput('upload-ref', authorized ? inspection.uploadRef : '');
    core.setOutput('upload-sha', authorized ? inspection.uploadSha : '');
}

async function authorize(args) {
    const inspection = await inspect(args);
    const errors = [...inspection.errors];
    if ((process.env.REPORTER_TEST_OUTCOME || '') !== 'success') {
        errors.push('Trusted reporter regression tests did not pass.');
    }

    const mode = process.env.REPORT_MODE;
    if (mode === 'trusted-final') {
        if ((process.env.DOWNLOAD_OUTCOME || '') !== 'success') {
            errors.push('Exact artifact download did not pass.');
        }
        if ((process.env.SARIF_VALIDATION_OUTCOME || '') !== 'success') {
            errors.push('Trusted SARIF validation did not pass.');
        }
        try {
            const expected = JSON.parse(process.env.PREFLIGHT_ARTIFACT_MANIFEST || '');
            if (JSON.stringify(expected) !== JSON.stringify(inspection.manifest)) {
                errors.push('Artifact ID, digest, size, or provenance changed after preflight.');
            }
        } catch (error) {
            errors.push(`Preflight artifact manifest is invalid: ${error.message}`);
        }
        if ((process.env.PREFLIGHT_UPLOAD_REF || '') !== inspection.uploadRef ||
            normalizeSha(process.env.PREFLIGHT_UPLOAD_SHA) !== normalizeSha(inspection.uploadSha)) {
            errors.push('Upload ref/SHA changed after preflight.');
        }
    } else if (mode !== 'trusted-preflight') {
        errors.push(`Unsupported reporter mode '${mode || 'unset'}'.`);
    }

    const authorized = errors.length === 0;
    setOutputs(args.core, inspection, authorized);
    if (authorized) {
        args.core.info(`Authorized immutable Codacy artifact ${inspection.manifest.id} from run ${inspection.run.id}.`);
    } else {
        args.core.setFailed(`Codacy trusted authorization failed: ${[...new Set(errors)].join(' ')}`);
    }
    return { ...inspection, errors: [...new Set(errors)], authorized };
}

authorize._test = {
    ARTIFACT_NAME, SOURCE_WORKFLOW_NAME, SOURCE_WORKFLOW_PATH, TRUSTED_PATHS,
    artifactManifest, exactRunMatches, pullMatches, workflowPath
};

module.exports = authorize;
