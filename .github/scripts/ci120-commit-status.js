const fs = require('fs');
const path = require('path');

const STATUS_CONTEXT = 'CI-120 Trusted / Exact Source';
const AGGREGATE_STATUS_CONTEXT = 'Trusted Exact-Source CI / Aggregate';
const SOURCE_WORKFLOW_NAME = 'Build SparkEngine';
const SOURCE_WORKFLOW_PATH = '.github/workflows/build.yml';
const VERIFIER_WORKFLOW_PATH = '.github/workflows/ci120-report.yml';
const SOURCE_JOB_NAME = 'Windows Shipping structural configured-evidence producer';
const SOURCE_FINAL_STEP = 'Enforce reviewed CI-120 findings';
const AUTHORITY = 'github-actions-protected-workflow-run-v1';
const SHA_PATTERN = /^[0-9a-f]{40}$/i;
const DIGEST_PATTERN = /^sha256:[0-9a-f]{64}$/i;
const RAW_DIGEST_PATTERN = /^[0-9a-f]{64}$/i;
const EXPECTED_PROFILES = Object.freeze([
    'installed-sdk-consumer',
    'windows-shipping',
    'windows-validation'
]);
const MAX_EXTRACTED_FILES = 100000;
const MAX_EXTRACTED_BYTES = 8 * 1024 * 1024 * 1024;
const STATUS_EVENTS = Object.freeze(['push', 'workflow_dispatch']);
const INVENTORY_EVENTS = Object.freeze([...STATUS_EVENTS, 'pull_request', 'schedule']);
const MAX_RUNS = 200;
const MAX_RUN_PAGES = Math.ceil(MAX_RUNS / 100);
const MAX_JSON_BYTES = 8 * 1024 * 1024;
const DESCRIPTION_PREFIXES = Object.freeze({
    pending: 'Trusted CI-120 verification running',
    success: 'Trusted CI-120 verified',
    failure: 'Trusted CI-120 incomplete'
});

const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const normalizedSha = value => SHA_PATTERN.test(value || '') ? value.toLowerCase() : null;
const unique = values => [...new Set(values.filter(Boolean))];

function normalizeWorkflowPath(value) {
    const text = String(value || '').split('@', 1)[0].replace(/\\/g, '/');
    const marker = '/.github/workflows/';
    const offset = text.indexOf(marker);
    return offset >= 0 ? text.slice(offset + 1) : text.replace(/^\.\//, '');
}

function exactRepository(candidate, expected) {
    return isObject(candidate) && isObject(expected) &&
        Number.isInteger(candidate.id) && candidate.id === expected.id &&
        candidate.full_name === expected.full_name;
}

function readBoundedJson(file, label) {
    if (typeof file !== 'string' || !file) throw new Error(`${label} path is missing.`);
    const stat = fs.lstatSync(file);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size < 2 || stat.size > MAX_JSON_BYTES) {
        throw new Error(`${label} is not one bounded regular file.`);
    }
    const parsed = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (!isObject(parsed)) throw new Error(`${label} root is not an object.`);
    return parsed;
}

function responseRuns(response, normalized = false) {
    const runs = normalized && Array.isArray(response?.data)
        ? response.data : response?.data?.workflow_runs;
    if (!Array.isArray(runs)) throw new Error('The same-commit workflow inventory is malformed.');
    return runs;
}

function exactSinglePageInventory(response, key, label) {
    const items = response?.data?.[key];
    const total = response?.data?.total_count;
    if (!Array.isArray(items) || !Number.isInteger(total) || total !== items.length || items.length > 100) {
        throw new Error(`${label} is incomplete or malformed.`);
    }
    return items;
}

function artifactWorkflowRunMatches(artifact, run, repository) {
    const provenance = artifact?.workflow_run;
    const artifactSha = normalizedSha(provenance?.head_sha);
    return isObject(provenance) && isObject(run) && isObject(repository) &&
        provenance.id === run.id && provenance.repository_id === repository.id &&
        provenance.head_repository_id === run.head_repository?.id &&
        provenance.head_branch === run.head_branch && artifactSha !== null &&
        artifactSha === normalizedSha(run.head_sha);
}

async function authorizeSourceJobArtifact({ github, context, run, repository, sourceSha }) {
    if (!isObject(run) || !isObject(repository) || normalizedSha(sourceSha) !== normalizedSha(run.head_sha)) {
        throw new Error('Source authorization inputs are malformed or inconsistent.');
    }
    const request = { ...context.repo, run_id: run.id };
    const jobsResponse = await github.rest.actions.listJobsForWorkflowRunAttempt({
        ...request, attempt_number: run.run_attempt, per_page: 100, page: 1
    });
    const jobs = exactSinglePageInventory(jobsResponse, 'jobs', 'Source job inventory');
    const jobIds = new Set();
    for (const job of jobs) {
        if (!isObject(job) || !Number.isInteger(job.id) || job.id < 1 ||
            typeof job.name !== 'string' || !job.name.trim() || jobIds.has(job.id)) {
            throw new Error('Source job inventory contains a malformed or duplicate job identity.');
        }
        jobIds.add(job.id);
    }
    const sourceJobs = jobs.filter(job => job?.name === SOURCE_JOB_NAME);
    if (sourceJobs.length !== 1) throw new Error('Expected exactly one CI-120 source job.');
    const sourceJob = sourceJobs[0];
    if (!Number.isInteger(sourceJob.id) || sourceJob.id < 1 || sourceJob.status !== 'completed' ||
        sourceJob.conclusion !== 'failure' || !Array.isArray(sourceJob.steps)) {
        throw new Error('CI-120 source job is not the exact completed fail-closed job.');
    }
    const expectedRunUrl = `https://api.github.com/repos/${repository.full_name}/actions/runs/${run.id}`;
    if (sourceJob.run_id !== run.id || sourceJob.run_attempt !== run.run_attempt ||
        normalizedSha(sourceJob.head_sha) !== normalizedSha(run.head_sha) ||
        sourceJob.head_branch !== run.head_branch || sourceJob.workflow_name !== run.name ||
        sourceJob.run_url !== expectedRunUrl) {
        throw new Error('Source job provenance does not bind the exact source run and commit.');
    }
    const requiredSuccessfulSteps = [
        'Checkout repository',
        'Setup MSVC',
        'Configure and build canonical Windows Shipping lane (CI-120)',
        'Capture Windows Shipping structural provenance (CI-120)',
        'Install exact Windows Shipping SDK for the consumer profile (CI-120)',
        'Configure and build canonical Windows validation lane (CI-120)',
        'Capture Windows validation structural provenance (CI-120)',
        'Configure and build installed SDK consumer lane (CI-120)',
        'Capture installed SDK consumer structural provenance (CI-120)',
        'Generate configured build-matrix inventory from the structural producer (CI-120)',
        'Compare reviewed configured-evidence findings (CI-120)',
        'Upload untrusted CI-120 structural evidence'
    ];
    const named = name => sourceJob.steps.filter(step => step?.name === name);
    for (const name of requiredSuccessfulSteps) {
        const matches = named(name);
        if (matches.length !== 1 || matches[0].status !== 'completed' || matches[0].conclusion !== 'success') {
            throw new Error(`Required CI-120 source step did not succeed exactly once: ${name}`);
        }
    }
    const finalSteps = named(SOURCE_FINAL_STEP);
    if (finalSteps.length !== 1 || finalSteps[0].status !== 'completed' ||
        finalSteps[0].conclusion !== 'failure') {
        throw new Error('CI-120 did not fail only at its documented external-authority enforcement step.');
    }
    if (sourceJob.steps.some(step => step?.conclusion === 'failure' && step?.name !== SOURCE_FINAL_STEP)) {
        throw new Error('CI-120 source job has an unexpected failed step.');
    }

    const artifactsResponse = await github.rest.actions.listWorkflowRunArtifacts({
        ...request, per_page: 100, page: 1
    });
    const artifacts = exactSinglePageInventory(artifactsResponse, 'artifacts', 'Source artifact inventory');
    const expectedArtifactName = `ci120-untrusted-stable-v1-${sourceSha}-${run.run_attempt}`;
    const candidates = artifacts.filter(artifact => artifact?.name === expectedArtifactName);
    if (candidates.length !== 1) throw new Error('Expected exactly one exact-attempt CI-120 source artifact.');
    const artifact = candidates[0];
    if (!Number.isInteger(artifact.id) || artifact.id < 1 || artifact.expired !== false ||
        !Number.isInteger(artifact.size_in_bytes) || artifact.size_in_bytes < 1 ||
        artifact.size_in_bytes > 4 * 1024 * 1024 * 1024 || !DIGEST_PATTERN.test(artifact.digest || '')) {
        throw new Error('CI-120 artifact identity, retention, size, or API digest is invalid.');
    }
    if (!artifactWorkflowRunMatches(artifact, run, repository)) {
        throw new Error('CI-120 artifact is not tied to the exact source run, repository, branch, and commit.');
    }
    return { sourceJob, finalStep: finalSteps[0], artifact };
}

function boundedPositiveInteger(value, maximum) {
    return Number.isSafeInteger(value) && value > 0 && value <= maximum;
}

function boundedNonNegativeInteger(value, maximum) {
    return Number.isSafeInteger(value) && value >= 0 && value <= maximum;
}

function receiptHasSemanticClosure(receipt) {
    const inputArtifact = receipt?.inputArtifact;
    const profiles = receipt?.profiles;
    const pendingState = receipt?.pendingState;
    if (!isObject(inputArtifact) || !Array.isArray(profiles) ||
        profiles.length !== EXPECTED_PROFILES.length || !isObject(pendingState) ||
        !boundedPositiveInteger(inputArtifact.extractedFileCount, MAX_EXTRACTED_FILES) ||
        !boundedPositiveInteger(inputArtifact.extractedBytes, MAX_EXTRACTED_BYTES) ||
        !RAW_DIGEST_PATTERN.test(inputArtifact.inventorySha256 || '') ||
        !RAW_DIGEST_PATTERN.test(inputArtifact.parityReportSha256 || '') ||
        pendingState.state !== 'pending-external-attestation' ||
        pendingState.authority !== 'external-attestation-required' ||
        pendingState.errorCount !== EXPECTED_PROFILES.length || pendingState.warningCount !== 2) {
        return false;
    }
    return profiles.every((profile, index) => isObject(profile) &&
        profile.id === EXPECTED_PROFILES[index] &&
        RAW_DIGEST_PATTERN.test(profile.recordSha256 || '') &&
        RAW_DIGEST_PATTERN.test(profile.replyDigest || '') &&
        RAW_DIGEST_PATTERN.test(profile.artifactManifestSha256 || '') &&
        boundedNonNegativeInteger(profile.targetCount, Number.MAX_SAFE_INTEGER) &&
        boundedNonNegativeInteger(profile.artifactCount, Number.MAX_SAFE_INTEGER));
}

async function listSameCommitRuns(github, request) {
    const runs = [];
    if (typeof github.paginate?.iterator === 'function') {
        let totalCount = null;
        let pageCount = 0;
        for await (const response of github.paginate.iterator(
            github.rest.actions.listWorkflowRuns, { ...request, per_page: 100 }
        )) {
            pageCount += 1;
            if (pageCount > MAX_RUN_PAGES) {
                throw new Error('The same-commit workflow inventory pagination did not terminate within its bound.');
            }
            const page = responseRuns(response, true);
            const observedTotal = response?.data?.total_count;
            if (!Number.isSafeInteger(observedTotal) || observedTotal < 0 || observedTotal > MAX_RUNS) {
                throw new Error(`The same-commit workflow inventory total_count is invalid or exceeds ${MAX_RUNS} runs.`);
            }
            if (totalCount === null) totalCount = observedTotal;
            else if (totalCount !== observedTotal) {
                throw new Error('The same-commit workflow inventory total_count changed between pages.');
            }
            if (page.length === 0 && runs.length < totalCount) {
                throw new Error('The same-commit workflow inventory pagination made no progress.');
            }
            if (runs.length + page.length > totalCount) {
                throw new Error('The same-commit workflow inventory returned more runs than total_count.');
            }
            runs.push(...page);
        }
        if (totalCount === null || runs.length !== totalCount) {
            throw new Error('The same-commit workflow inventory is incomplete: returned runs do not match total_count.');
        }
        return runs;
    }
    const response = await github.rest.actions.listWorkflowRuns({ ...request, per_page: 100, page: 1 });
    const page = responseRuns(response);
    if (!Number.isInteger(response.data.total_count) || response.data.total_count !== page.length ||
        page.length > 100) {
        throw new Error('The unpaginated same-commit workflow inventory is incomplete or malformed.');
    }
    return page;
}

function runMatches(run, expected, repository) {
    return isObject(run) && isObject(expected) &&
        run.id === expected.id && run.workflow_id === expected.workflow_id &&
        run.run_number === expected.run_number && run.run_attempt === expected.run_attempt &&
        run.name === SOURCE_WORKFLOW_NAME && normalizeWorkflowPath(run.path) === SOURCE_WORKFLOW_PATH &&
        run.event === expected.event && run.status === expected.status &&
        run.conclusion === expected.conclusion && run.head_branch === 'Working' &&
        normalizedSha(run.head_sha) === normalizedSha(expected.head_sha) &&
        exactRepository(run.repository, repository) && exactRepository(run.head_repository, repository);
}

function sameCommitInventoryReasons(runs, eventRun, sourceSha) {
    const reasons = [];
    const seen = new Set();
    const seenExecutions = new Set();
    for (const candidate of runs) {
        const execution = `${candidate?.run_number}:${candidate?.run_attempt}`;
        if (!isObject(candidate) || !Number.isInteger(candidate.id) || candidate.id < 1 ||
            candidate.workflow_id !== eventRun.workflow_id ||
            !Number.isInteger(candidate.run_number) || candidate.run_number < 1 ||
            !Number.isInteger(candidate.run_attempt) || candidate.run_attempt < 1 ||
            !INVENTORY_EVENTS.includes(candidate.event) || normalizedSha(candidate.head_sha) !== sourceSha ||
            seen.has(candidate.id) || seenExecutions.has(execution)) {
            reasons.push('The same-commit Build workflow inventory is malformed or not exact.');
            break;
        }
        seen.add(candidate.id);
        seenExecutions.add(execution);
        if (STATUS_EVENTS.includes(candidate.event) &&
            (candidate.run_number > eventRun.run_number ||
                candidate.run_number === eventRun.run_number &&
                candidate.run_attempt > eventRun.run_attempt)) {
            reasons.push('A newer CI-120 source run or attempt exists for this exact commit.');
        }
    }
    if (!seen.has(eventRun.id)) reasons.push('The exact CI-120 source run is missing from its workflow inventory.');
    return unique(reasons);
}

async function inspectSource({ github, context }, mode) {
    const reasons = [];
    const event = context.payload || {};
    const eventRun = event.workflow_run;
    const owner = context.repo.owner;
    const repo = context.repo.repo;
    const actionAccepted = event.action === 'completed' || mode === 'pending' && event.action === 'in_progress';
    if (!actionAccepted || !isObject(eventRun) || !Number.isInteger(eventRun.id) || eventRun.id < 1 ||
        !Number.isInteger(eventRun.workflow_id) || eventRun.workflow_id < 1 ||
        !Number.isInteger(eventRun.run_number) || eventRun.run_number < 1 ||
        !Number.isInteger(eventRun.run_attempt) || eventRun.run_attempt < 1 ||
        !STATUS_EVENTS.includes(eventRun.event) || eventRun.name !== SOURCE_WORKFLOW_NAME ||
        normalizeWorkflowPath(eventRun.path) !== SOURCE_WORKFLOW_PATH ||
        eventRun.head_branch !== 'Working' || !normalizedSha(eventRun.head_sha)) {
        reasons.push('The workflow_run event is not an exact supported CI-120 source attempt.');
    }
    const sourceSha = normalizedSha(eventRun?.head_sha);

    const repository = (await github.rest.repos.get({ owner, repo })).data;
    if (!isObject(repository) || repository.default_branch !== 'Working' ||
        repository.full_name !== `${owner}/${repo}` || !Number.isInteger(repository.id) || repository.id < 1 ||
        !exactRepository(event.repository, repository)) {
        reasons.push('The event and API repository do not identify the Working default branch exactly.');
    }
    if (!exactRepository(eventRun?.repository, repository) ||
        !exactRepository(eventRun?.head_repository, repository)) {
        reasons.push('The CI-120 source run is not owned by the base repository.');
    }

    const checkoutSha = normalizedSha(process.env.TRUSTED_CHECKOUT_SHA);
    const workflowSha = normalizedSha(process.env.TRUSTED_WORKFLOW_SHA);
    const expectedWorkflowRef = `${repository.full_name}/${VERIFIER_WORKFLOW_PATH}@refs/heads/Working`;
    const defaultCommit = (await github.rest.repos.getCommit({ owner, repo, ref: 'Working' })).data;
    if (!sourceSha || checkoutSha !== workflowSha ||
        normalizedSha(defaultCommit?.sha) !== checkoutSha ||
        process.env.TRUSTED_WORKFLOW_REF !== expectedWorkflowRef) {
        reasons.push('Verifier checkout, workflow, and current Working commit are not exact.');
    }

    let run = null;
    if (Number.isInteger(eventRun?.id) && eventRun.id > 0) {
        run = (await github.rest.actions.getWorkflowRun({ owner, repo, run_id: eventRun.id })).data;
    }
    if (!runMatches(run, eventRun, repository)) {
        reasons.push('The Actions API source run changed or is not the trusted Build workflow.');
    }
    if (sourceSha && checkoutSha) {
        try {
            const [sourceContent, trustedContent] = await Promise.all([
                github.rest.repos.getContent({ owner, repo, path: SOURCE_WORKFLOW_PATH, ref: sourceSha }),
                github.rest.repos.getContent({ owner, repo, path: SOURCE_WORKFLOW_PATH, ref: checkoutSha })
            ]);
            const sourceFile = sourceContent.data;
            const trustedFile = trustedContent.data;
            if (!isObject(sourceFile) || !isObject(trustedFile) ||
                sourceFile.type !== 'file' || trustedFile.type !== 'file' ||
                !normalizedSha(sourceFile.sha) || !normalizedSha(trustedFile.sha) ||
                normalizedSha(sourceFile.sha) !== normalizedSha(trustedFile.sha)) {
                reasons.push('The source Build workflow is not byte-identical to the trusted verifier workflow.');
            }
        } catch (error) {
            reasons.push(`The source Build workflow could not be attested: ${error.message}`);
        }
    }
    const expectedStatus = event.action === 'in_progress' ? 'in_progress' : 'completed';
    if (eventRun?.status !== expectedStatus || run?.status !== expectedStatus) {
        reasons.push(`The source run is not in the expected '${expectedStatus}' lifecycle state.`);
    }
    if (mode === 'final' && event.action !== 'completed') {
        reasons.push('A final CI-120 status requires a completed source event.');
    }

    let sameCommitRuns = [];
    if (sourceSha && Number.isInteger(eventRun?.workflow_id)) {
        sameCommitRuns = await listSameCommitRuns(github, {
            owner, repo, workflow_id: eventRun.workflow_id, head_sha: sourceSha
        });
        reasons.push(...sameCommitInventoryReasons(sameCommitRuns, eventRun, sourceSha));
    }

    const commit = sourceSha
        ? (await github.rest.repos.getCommit({ owner, repo, ref: sourceSha })).data : null;
    if (!sourceSha || normalizedSha(commit?.sha) !== sourceSha) {
        reasons.push('The base repository cannot resolve the exact CI-120 source commit.');
    }

    return { owner, repo, repository, event, eventRun, run, sourceSha, reasons: unique(reasons) };
}

function reporterTargetUrl(inspection) {
    const runId = Number(process.env.GITHUB_RUN_ID);
    const attempt = Number(process.env.GITHUB_RUN_ATTEMPT);
    if (!Number.isSafeInteger(runId) || runId < 1 || !Number.isSafeInteger(attempt) || attempt < 1) {
        throw new Error('The trusted verifier run ID or attempt is invalid.');
    }
    return `https://github.com/${inspection.owner}/${inspection.repo}/actions/runs/${runId}/attempts/${attempt}`;
}

async function publishStatus(args, mode, state) {
    if (!Object.hasOwn(DESCRIPTION_PREFIXES, state)) throw new Error(`Unsupported CI-120 status '${state}'.`);
    const inspection = await inspectSource(args, mode);
    if (inspection.reasons.length) {
        return { performed: false, reason: 'stale-or-untrusted-source', staleReasons: inspection.reasons,
            targetSha: inspection.sourceSha, state: null, targetUrl: null, context: STATUS_CONTEXT };
    }
    const immediate = (await args.github.rest.actions.getWorkflowRun({
        owner: inspection.owner, repo: inspection.repo, run_id: inspection.run.id
    })).data;
    if (!runMatches(immediate, inspection.run, inspection.repository)) {
        return { performed: false, reason: 'stale-or-untrusted-source',
            staleReasons: ['The CI-120 source run changed in the final status mutation window.'],
            targetSha: inspection.sourceSha, state: null, targetUrl: null, context: STATUS_CONTEXT };
    }
    const immediateRuns = await listSameCommitRuns(args.github, {
        owner: inspection.owner,
        repo: inspection.repo,
        workflow_id: inspection.eventRun.workflow_id,
        head_sha: inspection.sourceSha
    });
    const immediateReasons = sameCommitInventoryReasons(
        immediateRuns, inspection.eventRun, inspection.sourceSha);
    if (immediateReasons.length) {
        return { performed: false, reason: 'stale-or-untrusted-source', staleReasons: immediateReasons,
            targetSha: inspection.sourceSha, state: null, targetUrl: null, context: STATUS_CONTEXT };
    }
    const targetUrl = reporterTargetUrl(inspection);
    const description = `${DESCRIPTION_PREFIXES[state]} for Build run ${inspection.eventRun.id}, ` +
        `attempt ${inspection.eventRun.run_attempt}.`;
    await args.github.rest.repos.createCommitStatus({
        owner: inspection.owner,
        repo: inspection.repo,
        sha: inspection.sourceSha,
        state: 'pending',
        target_url: targetUrl,
        description: `Trusted exact-source aggregate is awaiting both reporters for ${inspection.sourceSha.slice(0, 12)}.`,
        context: AGGREGATE_STATUS_CONTEXT
    });
    await args.github.rest.repos.createCommitStatus({
        owner: inspection.owner,
        repo: inspection.repo,
        sha: inspection.sourceSha,
        state,
        target_url: targetUrl,
        description,
        context: STATUS_CONTEXT
    });
    return { performed: true, reason: 'published', targetSha: inspection.sourceSha,
        state, targetUrl, context: STATUS_CONTEXT };
}

function finalEvidenceErrors(eventRun) {
    const errors = [];
    const outcomes = [
        ['verifier tests', process.env.VERIFIER_TEST_OUTCOME],
        ['source preflight', process.env.PREFLIGHT_OUTCOME],
        ['artifact download', process.env.DOWNLOAD_OUTCOME],
        ['evidence verification', process.env.VERIFY_EVIDENCE_OUTCOME],
        ['receipt attestation', process.env.ATTEST_RECEIPT_OUTCOME],
        ['receipt upload', process.env.UPLOAD_RECEIPT_OUTCOME]
    ];
    for (const [label, outcome] of outcomes) {
        if (outcome !== 'success') errors.push(`${label} concluded '${outcome || 'unknown'}'.`);
    }
    if (errors.length) return errors;

    try {
        const metadata = readBoundedJson(process.env.SOURCE_METADATA_PATH, 'CI-120 source metadata');
        const receipt = readBoundedJson(process.env.RECEIPT_PATH, 'CI-120 trusted receipt');
        const source = receipt.source;
        const metadataSource = metadata.source;
        const verifier = receipt.verifier;
        const inputArtifact = receipt.inputArtifact;
        if (receipt.schemaVersion !== 1 || receipt.kind !== 'spark-ci120-trusted-workflow-run' ||
            receipt.state !== 'verified' || receipt.authority !== AUTHORITY || receipt.profile !== 'stable-v1' ||
            !isObject(source) || !isObject(metadataSource) ||
            source.repository !== metadata.repository?.fullName ||
            source.workflowId !== eventRun.workflow_id || source.workflowName !== SOURCE_WORKFLOW_NAME ||
            source.workflowPath !== SOURCE_WORKFLOW_PATH || source.runId !== eventRun.id ||
            source.runNumber !== eventRun.run_number || source.runAttempt !== eventRun.run_attempt ||
            source.event !== eventRun.event || source.conclusion !== 'failure' ||
            source.headBranch !== 'Working' || normalizedSha(source.headSha) !== normalizedSha(eventRun.head_sha) ||
            source.jobName !== SOURCE_JOB_NAME || source.jobConclusion !== 'failure' ||
            source.expectedFailClosedStep !== SOURCE_FINAL_STEP) {
            errors.push('The trusted receipt does not bind the exact expected CI-120 source execution.');
        }
        if (metadataSource?.runId !== eventRun.id || metadataSource?.runNumber !== eventRun.run_number ||
            metadataSource?.runAttempt !== eventRun.run_attempt ||
            normalizedSha(metadataSource?.headSha) !== normalizedSha(eventRun.head_sha) ||
            metadataSource?.jobId !== source?.jobId || metadataSource?.jobName !== source?.jobName ||
            metadataSource?.finalStepName !== SOURCE_FINAL_STEP || metadataSource?.finalStepConclusion !== 'failure') {
            errors.push('The source metadata and trusted receipt do not identify the same source job and attempt.');
        }
        if (!isObject(verifier) || verifier.repository !== metadata.repository?.fullName ||
            verifier.workflowRef !== process.env.TRUSTED_WORKFLOW_REF ||
            normalizedSha(verifier.workflowSha) !== normalizedSha(process.env.TRUSTED_WORKFLOW_SHA) ||
            normalizedSha(verifier.checkoutSha) !== normalizedSha(process.env.TRUSTED_CHECKOUT_SHA) ||
            !normalizedSha(verifier.sourceWorkflowBlobSha) ||
            normalizedSha(verifier.sourceWorkflowBlobSha) !== normalizedSha(verifier.trustedWorkflowBlobSha) ||
            normalizedSha(verifier.sourceWorkflowBlobSha) !== normalizedSha(metadata.verifier?.sourceWorkflowBlobSha) ||
            normalizedSha(verifier.trustedWorkflowBlobSha) !== normalizedSha(metadata.verifier?.trustedWorkflowBlobSha)) {
            errors.push('The receipt verifier identity does not match the executing trusted verifier.');
        }
        if (!isObject(inputArtifact) || inputArtifact.id !== metadata.artifact?.id ||
            inputArtifact.name !== metadata.artifact?.name || inputArtifact.bytes !== metadata.artifact?.bytes ||
            !DIGEST_PATTERN.test(inputArtifact.digest || '') ||
            inputArtifact.digest.toLowerCase() !== String(metadata.artifact?.digest || '').toLowerCase()) {
            errors.push('The receipt input artifact does not match the exact API-authorized source artifact.');
        }
        if (!receiptHasSemanticClosure(receipt)) {
            errors.push('The trusted receipt lacks exact semantic closure for profiles, pending state, digests, or extracted bounds.');
        }
    } catch (error) {
        errors.push(`Trusted receipt binding failed: ${error.message}`);
    }
    return unique(errors);
}

function writeRecord(record) {
    const output = process.env.STATUS_RECORD_PATH;
    if (!output) return;
    fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
    fs.writeFileSync(output, `${JSON.stringify(record, null, 2)}\n`, { encoding: 'utf8', flag: 'wx' });
}

async function pending(args) {
    try {
        const result = await publishStatus(args, 'pending', 'pending');
        args.core.setOutput('status-published', result.performed ? 'true' : 'false');
        args.core.setOutput('status-target-sha', result.performed ? result.targetSha : '');
        if (!result.performed) {
            args.core.setFailed(`CI-120 pending status was not published: ${result.staleReasons.join(' ')}`);
        }
        return result;
    } catch (error) {
        args.core.setOutput('status-published', 'false');
        args.core.setOutput('status-target-sha', '');
        args.core.setFailed(`CI-120 pending status API failed: ${error.message}`);
        return { performed: false, reason: 'api-error', error: error.message };
    }
}

async function final(args) {
    const evidenceErrors = finalEvidenceErrors(args.context.payload?.workflow_run || {});
    const desiredState = evidenceErrors.length ? 'failure' : 'success';
    let result;
    try {
        result = await publishStatus(args, 'final', desiredState);
        if (!result.performed) {
            args.core.setFailed(`CI-120 final status was not published: ${result.staleReasons.join(' ')}`);
        } else if (evidenceErrors.length) {
            args.core.setFailed(`CI-120 trusted evidence is incomplete: ${evidenceErrors.join(' ')}`);
        }
    } catch (error) {
        result = { performed: false, reason: 'api-error', error: error.message,
            targetSha: normalizedSha(args.context.payload?.workflow_run?.head_sha),
            state: null, targetUrl: null, context: STATUS_CONTEXT };
        args.core.setFailed(`CI-120 final status API failed: ${error.message}`);
    }
    const record = {
        schemaVersion: 1,
        kind: 'spark-ci120-trusted-status',
        generatedAt: new Date().toISOString(),
        evidenceState: evidenceErrors.length ? 'incomplete' : 'verified',
        evidenceErrors,
        commitStatus: result
    };
    try { writeRecord(record); }
    catch (error) { args.core.setFailed(`Could not write CI-120 status record: ${error.message}`); }
    return record;
}

module.exports = async args => {
    const mode = (process.env.CI120_STATUS_MODE || '').trim();
    if (mode === 'pending') return pending(args);
    if (mode === 'final') return final(args);
    throw new Error(`Unsupported CI120_STATUS_MODE '${mode || '<empty>'}'.`);
};

module.exports.authorizeSourceJobArtifact = authorizeSourceJobArtifact;

module.exports._test = Object.freeze({
    artifactWorkflowRunMatches,
    authorizeSourceJobArtifact,
    exactSinglePageInventory,
    finalEvidenceErrors,
    normalizeWorkflowPath,
    receiptHasSemanticClosure
});
