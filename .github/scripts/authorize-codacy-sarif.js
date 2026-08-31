// Trusted workflow_run authorization for Codacy SARIF upload.
//
// This file is executed only from a verified default-branch checkout. It never
// loads source-run code or downloaded artifact content.

const fs = require('fs');

const SOURCE_WORKFLOW_NAME = 'Codacy Security Scan';
const SOURCE_WORKFLOW_PATH = '.github/workflows/codacy.yml';
const REPORTER_WORKFLOW_PATH = '.github/workflows/codacy-report.yml';
const ARTIFACT_NAME = 'results.sarif';
const SOURCE_JOB_NAME = 'Codacy Security Scan';
const ARTIFACT_UPLOAD_STEP = 'Publish normalized SARIF artifact';
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
const MAX_LIST_PAGES = MAX_LIST_ITEMS;

const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const normalizeSha = value => SHA_PATTERN.test(value || '') ? value.toLowerCase() : null;
const workflowPath = value => typeof value === 'string' ? value.split('@')[0] : '';

function timestamp(value, label) {
    if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$/.test(value)) {
        throw new Error(`${label} is not an exact UTC timestamp.`);
    }
    const parsed = Date.parse(value);
    if (!Number.isFinite(parsed)) throw new Error(`${label} is not an exact UTC timestamp.`);
    return parsed;
}

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

function responseItems(response, field, normalized = false) {
    const items = normalized && Array.isArray(response?.data)
        ? response.data : field ? response?.data?.[field] : response?.data;
    if (!Array.isArray(items)) throw new Error(`${field || 'root'} ${field ? 'field' : 'response'} is not an array.`);
    return items;
}

async function listBounded(github, method, request, field, label) {
    const items = [];
    const requireExactTotal = Boolean(field);
    let exactTotal = null;
    const observeTotal = response => {
        if (!requireExactTotal) return;
        const total = response?.data?.total_count;
        if (!Number.isSafeInteger(total) || total < 0 || total > MAX_LIST_ITEMS) {
            throw new Error(`${label} total_count is invalid or exceeds ${MAX_LIST_ITEMS} items.`);
        }
        if (exactTotal === null) exactTotal = total;
        else if (exactTotal !== total) throw new Error(`${label} total_count changed between pages.`);
    };
    const finish = () => {
        if (requireExactTotal && exactTotal !== items.length) {
            throw new Error(`${label} total_count does not match the returned inventory.`);
        }
        return items;
    };

    if (typeof github.paginate?.iterator === 'function') {
        let pageCount = 0;
        for await (const response of github.paginate.iterator(
            method, { ...request, per_page: MAX_LIST_ITEMS }
        )) {
            pageCount += 1;
            if (pageCount > MAX_LIST_PAGES) {
                throw new Error(`${label} pagination did not terminate within its bound.`);
            }
            const page = responseItems(response, field, true);
            observeTotal(response);
            if (page.length === 0 && (pageCount > 1 || requireExactTotal && items.length < exactTotal)) {
                throw new Error(`${label} pagination made no progress before completion.`);
            }
            if (items.length + page.length > MAX_LIST_ITEMS ||
                requireExactTotal && items.length + page.length > exactTotal) {
                throw new Error(`${label} exceeds its declared or ${MAX_LIST_ITEMS}-item authorization limit.`);
            }
            items.push(...page);
        }
        return finish();
    }

    const response = await method({ ...request, per_page: MAX_LIST_ITEMS, page: 1 });
    const page = responseItems(response, field);
    observeTotal(response);
    items.push(...page);
    if (items.length > MAX_LIST_ITEMS || String(response?.headers?.link || '').includes('rel="next"')) {
        throw new Error(`${label} exceeds the ${MAX_LIST_ITEMS}-item authorization limit.`);
    }
    return finish();
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
        const candidates = await listBounded(github, github.rest.pulls.list, {
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

async function attestLatestSourceRun(github, owner, repo, repository, run) {
    const runs = await listBounded(github, github.rest.actions.listWorkflowRuns, {
        owner, repo, workflow_id: run.workflow_id, head_sha: run.head_sha
    }, 'workflow_runs', 'same-head workflow-run list');
    const runIds = new Set();
    const executions = new Set();
    for (const candidate of runs) {
        if (!isObject(candidate) || !Number.isInteger(candidate.id) || candidate.id < 1 ||
            !Number.isInteger(candidate.run_number) || candidate.run_number < 1 ||
            !Number.isInteger(candidate.run_attempt) || candidate.run_attempt < 1 ||
            candidate.workflow_id !== run.workflow_id || candidate.name !== SOURCE_WORKFLOW_NAME ||
            workflowPath(candidate.path) !== SOURCE_WORKFLOW_PATH ||
            !['pull_request', 'push', 'schedule'].includes(candidate.event) ||
            normalizeSha(candidate.head_sha) !== normalizeSha(run.head_sha) ||
            !repositoryMatches(candidate.repository, owner, repo, repository?.id)) {
            throw new Error('same-head workflow-run inventory contains a malformed or mismatched run.');
        }
        const execution = `${candidate.run_number}:${candidate.run_attempt}`;
        if (runIds.has(candidate.id) || executions.has(execution)) {
            throw new Error('same-head workflow-run inventory contains a duplicate run identity.');
        }
        runIds.add(candidate.id);
        executions.add(execution);
    }
    const current = runs.filter(candidate => exactRunMatches(candidate, run));
    if (current.length !== 1) {
        throw new Error('Exact source workflow run is absent or duplicated in the same-head run inventory.');
    }
    if (runs.some(candidate => candidate.workflow_id === run.workflow_id &&
        normalizeSha(candidate.head_sha) === normalizeSha(run.head_sha) &&
        (candidate.run_number > run.run_number ||
            (candidate.run_number === run.run_number && candidate.run_attempt > run.run_attempt)))) {
        throw new Error('A newer source workflow run or attempt exists for this exact head.');
    }
    return runs;
}

function artifactManifest(artifact) {
    return {
        id: artifact?.id ?? null,
        nodeId: artifact?.node_id ?? null,
        name: artifact?.name ?? null,
        size: artifact?.size_in_bytes ?? null,
        digest: artifact?.digest ?? null,
        expired: artifact?.expired ?? null,
        createdAt: artifact?.created_at ?? null,
        updatedAt: artifact?.updated_at ?? null,
        expiresAt: artifact?.expires_at ?? null,
        url: artifact?.url ?? null,
        archiveDownloadUrl: artifact?.archive_download_url ?? null,
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
    let sourceJob = null;
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
            await attestLatestSourceRun(github, owner, repo, repository, run);
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
            const jobs = await listBounded(github, github.rest.actions.listJobsForWorkflowRunAttempt, {
                owner, repo, run_id: run.id, attempt_number: run.run_attempt
            }, 'jobs', 'exact source-attempt job inventory');
            const jobIds = new Set();
            for (const job of jobs) {
                if (!isObject(job) || !Number.isInteger(job.id) || job.id < 1 || jobIds.has(job.id) ||
                    typeof job.name !== 'string' || !job.name || job.run_id !== run.id ||
                    job.run_attempt !== run.run_attempt || normalizeSha(job.head_sha) !== normalizeSha(run.head_sha) ||
                    job.workflow_name !== SOURCE_WORKFLOW_NAME || job.head_branch !== run.head_branch ||
                    job.run_url !== `https://api.github.com/repos/${repository.full_name}/actions/runs/${run.id}`) {
                    throw new Error('exact source-attempt job inventory contains a malformed, duplicate, or mismatched job.');
                }
                jobIds.add(job.id);
            }
            const matches = jobs.filter(job => job.name === SOURCE_JOB_NAME);
            if (matches.length !== 1 || matches[0].status !== 'completed' || matches[0].conclusion !== 'success') {
                throw new Error(`Expected exactly one successful '${SOURCE_JOB_NAME}' source job.`);
            }
            sourceJob = matches[0];
            const runStarted = timestamp(run.run_started_at, 'source run_started_at');
            const runUpdated = timestamp(run.updated_at, 'source updated_at');
            const jobStarted = timestamp(sourceJob.started_at, 'source job started_at');
            const jobCompleted = timestamp(sourceJob.completed_at, 'source job completed_at');
            if (!(runStarted <= jobStarted && jobStarted <= jobCompleted && jobCompleted <= runUpdated)) {
                throw new Error('Source job is outside the exact source-run execution window.');
            }
            if (!Array.isArray(sourceJob.steps) || sourceJob.steps.some(step => !isObject(step))) {
                throw new Error('Source job step inventory is malformed.');
            }
            const uploadSteps = sourceJob.steps.filter(step => step.name === ARTIFACT_UPLOAD_STEP);
            if (uploadSteps.length !== 1 || uploadSteps[0].status !== 'completed' ||
                uploadSteps[0].conclusion !== 'success') {
                throw new Error(`Expected one successful '${ARTIFACT_UPLOAD_STEP}' source step.`);
            }
            const uploadStarted = timestamp(uploadSteps[0].started_at, 'artifact upload step started_at');
            const uploadCompleted = timestamp(uploadSteps[0].completed_at, 'artifact upload step completed_at');
            if (!(jobStarted <= uploadStarted && uploadStarted <= uploadCompleted && uploadCompleted <= jobCompleted)) {
                throw new Error('Artifact upload step is outside the exact source-job execution window.');
            }
        } catch (error) {
            errors.push(`Could not verify exact source-attempt job: ${error.message}`);
            sourceJob = null;
        }
    }

    if (run && repository) {
        try {
            const artifacts = await listBounded(github, github.rest.actions.listWorkflowRunArtifacts, {
                owner, repo, run_id: run.id
            }, 'artifacts', 'source artifact inventory');
            if (artifacts.length !== 1 || artifacts[0]?.name !== ARTIFACT_NAME) {
                errors.push(`Expected exactly one '${ARTIFACT_NAME}' artifact.`);
            } else {
                artifact = artifacts[0];
                const artifactId = artifact.id;
                const expectedUrl = `https://api.github.com/repos/${repository.full_name}/actions/artifacts/${artifactId}`;
                if (!Number.isInteger(artifactId) || artifactId < 1 ||
                    typeof artifact.node_id !== 'string' || !artifact.node_id || artifact.expired !== false ||
                    !Number.isInteger(artifact.size_in_bytes) || artifact.size_in_bytes < 1 ||
                    artifact.size_in_bytes > MAX_ARTIFACT_BYTES ||
                    !DIGEST_PATTERN.test(artifact.digest || '') || artifact.url !== expectedUrl ||
                    artifact.archive_download_url !== `${expectedUrl}/zip`) {
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
                try {
                    const created = timestamp(artifact.created_at, 'artifact created_at');
                    const updated = timestamp(artifact.updated_at, 'artifact updated_at');
                    const expires = timestamp(artifact.expires_at, 'artifact expires_at');
                    const runStarted = timestamp(run.run_started_at, 'source run_started_at');
                    const runUpdated = timestamp(run.updated_at, 'source updated_at');
                    if (!sourceJob) throw new Error('exact source upload step is unavailable.');
                    const uploadStep = sourceJob.steps.find(step => step.name === ARTIFACT_UPLOAD_STEP);
                    const uploadStarted = timestamp(uploadStep.started_at, 'artifact upload step started_at');
                    const uploadCompleted = timestamp(uploadStep.completed_at, 'artifact upload step completed_at');
                    if (!(runStarted <= created && created <= updated && updated <= runUpdated &&
                        uploadStarted - 1000 <= created && updated <= uploadCompleted + 1000 &&
                        expires > updated)) {
                        throw new Error('artifact is outside the exact upload/run window or already past its immutable lifetime.');
                    }
                } catch (error) {
                    errors.push(`Artifact '${ARTIFACT_NAME}' timing is invalid: ${error.message}`);
                }
            }
        } catch (error) {
            errors.push(`Could not inspect source artifact: ${error.message}`);
        }
    }

    if (!uploadRef || !normalizeSha(uploadSha)) errors.push('Upload ref/SHA provenance is unresolved.');
    return {
        errors: [...new Set(errors)], event, eventRun, repository, run, pull, artifact, sourceJob,
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

    if (mode === 'trusted-final' && errors.length === 0) {
        try {
            await attestLatestSourceRun(
                args.github,
                args.context.repo.owner,
                args.context.repo.repo,
                inspection.repository,
                inspection.run
            );
        } catch (error) {
            errors.push(`Could not reattest latest source workflow run: ${error.message}`);
        }
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
