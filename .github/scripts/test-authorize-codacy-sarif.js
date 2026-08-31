const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const authorize = require('./authorize-codacy-sarif.js');

const REPOSITORY_ID = 1001;
const HEAD_REPOSITORY_ID = 2002;
const RUN_ID = 3003;
const WORKFLOW_ID = 4004;
const ARTIFACT_ID = 5005;
const SOURCE_SHA = '1'.repeat(40);
const TRUSTED_SHA = '3'.repeat(40);
const SOURCE_WORKFLOW_BLOB = '4'.repeat(40);
const REPORTER_BLOB = '5'.repeat(40);
const DIGEST = `sha256:${'6'.repeat(64)}`;
const ENV_KEYS = [
    'REPORT_MODE', 'REPORTER_TEST_OUTCOME', 'TRUSTED_CHECKOUT_SHA',
    'TRUSTED_WORKFLOW_REF', 'TRUSTED_WORKFLOW_SHA',
    'WORKFLOW_RUN_EVENT_PATH', 'PREFLIGHT_ARTIFACT_MANIFEST',
    'PREFLIGHT_UPLOAD_REF', 'PREFLIGHT_UPLOAD_SHA', 'DOWNLOAD_OUTCOME',
    'SARIF_VALIDATION_OUTCOME'
];

const clone = value => JSON.parse(JSON.stringify(value));

function fixture() {
    const repository = {
        id: REPOSITORY_ID, full_name: 'Krilliac/SparkEngine', default_branch: 'Working'
    };
    const headRepository = { id: HEAD_REPOSITORY_ID, full_name: 'contributor/SparkEngine' };
    const pullReference = { number: 42 };
    const run = {
        id: RUN_ID,
        workflow_id: WORKFLOW_ID,
        run_number: 50,
        run_attempt: 1,
        name: 'Codacy Security Scan',
        path: '.github/workflows/codacy.yml@refs/pull/42/merge',
        event: 'pull_request',
        status: 'completed',
        conclusion: 'success',
        head_sha: SOURCE_SHA,
        head_branch: 'hostile-sarif',
        run_started_at: '2026-08-01T01:00:00Z',
        updated_at: '2026-08-01T01:10:00Z',
        repository,
        head_repository: headRepository,
        pull_requests: [pullReference]
    };
    const pull = {
        number: 42,
        state: 'open',
        base: { ref: 'Working', repo: repository },
        head: { ref: run.head_branch, sha: SOURCE_SHA, repo: headRepository }
    };
    const artifact = {
        id: ARTIFACT_ID,
        node_id: 'artifact-node-5005',
        name: 'results.sarif',
        size_in_bytes: 4096,
        expired: false,
        digest: DIGEST,
        created_at: '2026-08-01T01:08:00Z',
        updated_at: '2026-08-01T01:08:01Z',
        expires_at: '2026-08-02T01:08:01Z',
        url: `https://api.github.com/repos/Krilliac/SparkEngine/actions/artifacts/${ARTIFACT_ID}`,
        archive_download_url: `https://api.github.com/repos/Krilliac/SparkEngine/actions/artifacts/${ARTIFACT_ID}/zip`,
        workflow_run: {
            id: RUN_ID,
            repository_id: REPOSITORY_ID,
            head_repository_id: HEAD_REPOSITORY_ID,
            head_branch: run.head_branch,
            head_sha: SOURCE_SHA
        }
    };
    const job = {
        id: 6006,
        run_id: RUN_ID,
        run_attempt: 1,
        head_sha: SOURCE_SHA,
        workflow_name: 'Codacy Security Scan',
        head_branch: run.head_branch,
        run_url: `https://api.github.com/repos/Krilliac/SparkEngine/actions/runs/${RUN_ID}`,
        name: 'Codacy Security Scan',
        status: 'completed',
        conclusion: 'success',
        started_at: '2026-08-01T01:01:00Z',
        completed_at: '2026-08-01T01:09:00Z',
        steps: [{
            number: 5,
            name: 'Publish normalized SARIF artifact',
            status: 'completed',
            conclusion: 'success',
            started_at: '2026-08-01T01:07:00Z',
            completed_at: '2026-08-01T01:08:30Z'
        }]
    };
    return {
        repository,
        run,
        event: { action: 'completed', repository, workflow_run: clone(run) },
        pull,
        pulls: [pull],
        artifacts: [artifact],
        jobs: [job],
        runs: [run],
        defaultCommitSha: TRUSTED_SHA,
        sourceWorkflowBlob: SOURCE_WORKFLOW_BLOB,
        defaultSourceWorkflowBlob: SOURCE_WORKFLOW_BLOB,
        reporterBlob: REPORTER_BLOB
    };
}

function writeEvent(root, event) {
    const eventPath = path.join(root, `event-${Math.random().toString(16).slice(2)}.json`);
    fs.writeFileSync(eventPath, JSON.stringify(event), 'utf8');
    return eventPath;
}

async function withEnvironment(values, action) {
    const previous = Object.fromEntries(ENV_KEYS.map(key => [key, process.env[key]]));
    try {
        for (const key of ENV_KEYS) delete process.env[key];
        for (const [key, value] of Object.entries(values)) process.env[key] = String(value);
        return await action();
    } finally {
        for (const key of ENV_KEYS) {
            if (previous[key] === undefined) delete process.env[key];
            else process.env[key] = previous[key];
        }
    }
}

function harness(data) {
    const state = clone(data);
    const observed = { outputs: {}, failed: [], info: [], getContent: [], listRunRequests: [] };
    const github = {
        rest: {
            repos: {
                async get() { return { data: clone(state.repository) }; },
                async getCommit() { return { data: { sha: state.defaultCommitSha } }; },
                async getContent(request) {
                    observed.getContent.push(clone(request));
                    let sha = state.reporterBlob;
                    if (request.path === '.github/workflows/codacy.yml') {
                        sha = request.owner === 'Krilliac' && request.ref === 'Working'
                            ? state.defaultSourceWorkflowBlob : state.sourceWorkflowBlob;
                    }
                    return { data: { type: 'file', sha } };
                }
            },
            actions: {
                async getWorkflowRun() { return { data: clone(state.run) }; },
                async listWorkflowRuns(request) {
                    observed.listRunRequests.push(clone(request));
                    const responseIndex = observed.listRunRequests.length - 1;
                    const staged = state.runInventoryResponses;
                    const inventory = Array.isArray(staged) && staged.length
                        ? staged[Math.min(responseIndex, staged.length - 1)] : state.runs;
                    const runs = request.event
                        ? inventory.filter(run => run.event === request.event) : inventory;
                    return { data: {
                        total_count: state.runsTotalCount ?? runs.length,
                        workflow_runs: clone(runs)
                    }, headers: state.runsNextLink ? { link: '<next>; rel="next"' } : {} };
                },
                async listWorkflowRunArtifacts() {
                    return { data: {
                        total_count: state.artifactsTotalCount ?? state.artifacts.length,
                        artifacts: clone(state.artifacts)
                    }, headers: {} };
                },
                async listJobsForWorkflowRunAttempt() {
                    return { data: {
                        total_count: state.jobsTotalCount ?? state.jobs.length,
                        jobs: clone(state.jobs)
                    }, headers: {} };
                }
            },
            pulls: {
                async get() { return { data: clone(state.pull) }; },
                async list() { return { data: clone(state.pulls), headers: {} }; }
            }
        },
        paginate: {
            async *iterator(method, request) {
                const response = await method(request);
                if (Array.isArray(response.data)) {
                    yield response;
                    return;
                }
                const field = Array.isArray(response.data.workflow_runs) ? 'workflow_runs' :
                    Array.isArray(response.data.artifacts) ? 'artifacts' : 'jobs';
                if (state.normalizedPages?.[field]) {
                    for (const page of state.normalizedPages[field]) {
                        const normalizedPage = clone(page.items);
                        normalizedPage.total_count = page.total_count;
                        yield { ...response, data: normalizedPage };
                    }
                    return;
                }
                const normalized = clone(response.data[field]);
                normalized.total_count = response.data.total_count;
                yield { ...response, data: normalized };
            }
        }
    };
    if (state.useDirectListFallback) delete github.paginate;
    const core = {
        setOutput(name, value) { observed.outputs[name] = String(value); },
        setFailed(message) { observed.failed.push(message); },
        info(message) { observed.info.push(message); }
    };
    return { github, core, context: { repo: { owner: 'Krilliac', repo: 'SparkEngine' } }, observed };
}

async function run(root, data, values = {}) {
    const runtime = harness(data);
    const eventPath = writeEvent(root, data.event);
    const result = await withEnvironment({
        REPORT_MODE: 'trusted-preflight',
        REPORTER_TEST_OUTCOME: 'success',
        TRUSTED_CHECKOUT_SHA: TRUSTED_SHA,
        TRUSTED_WORKFLOW_REF: 'Krilliac/SparkEngine/.github/workflows/codacy-report.yml@refs/heads/Working',
        TRUSTED_WORKFLOW_SHA: TRUSTED_SHA,
        WORKFLOW_RUN_EVENT_PATH: eventPath,
        ...values
    }, () => authorize(runtime));
    return { runtime, result };
}

async function main() {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'codacy-auth-test-'));
    try {
        const cleanData = fixture();
        const clean = await run(root, cleanData);
        assert.strictEqual(clean.result.authorized, true);
        assert.strictEqual(clean.runtime.observed.outputs['artifact-id'], String(ARTIFACT_ID));
        assert.strictEqual(clean.runtime.observed.outputs['artifact-digest'], DIGEST);
        assert.strictEqual(clean.runtime.observed.outputs['upload-ref'], 'refs/pull/42/head');
        assert.strictEqual(clean.runtime.observed.outputs['upload-sha'], SOURCE_SHA);
        assert(clean.runtime.observed.getContent.some(request =>
            request.owner === 'contributor' && request.repo === 'SparkEngine' &&
            request.path === '.github/workflows/codacy.yml' && request.ref === SOURCE_SHA));
        for (const [shape, useDirectListFallback] of [['normalized', false], ['direct', true]]) {
            for (const inventory of ['jobs', 'runs', 'artifacts']) {
                const truncated = fixture();
                truncated.useDirectListFallback = useDirectListFallback;
                truncated[`${inventory}TotalCount`] = truncated[inventory].length + 1;
                const rejected = await run(root, truncated);
                assert.strictEqual(rejected.result.authorized, false,
                    `${shape} truncated ${inventory} inventory must fail preflight closed`);
                assert.strictEqual(rejected.runtime.observed.outputs['artifact-id'], '',
                    `${shape} truncated ${inventory} inventory must expose no downloadable artifact`);
                assert.strictEqual(rejected.runtime.observed.outputs['upload-sha'], '',
                    `${shape} truncated ${inventory} inventory must expose no privileged upload target`);
                assert(rejected.runtime.observed.failed.some(message => message.includes('total_count')),
                    `${shape} truncated ${inventory} inventory must report exact-count failure`);
            }
        }

        assert.strictEqual(clean.runtime.observed.listRunRequests[0].event, undefined,
            'same-SHA freshness must span every source event type');

        const newerCrossEvent = fixture();
        newerCrossEvent.runs.push({
            ...clone(newerCrossEvent.run), id: RUN_ID + 1, run_number: 51, event: 'schedule'
        });
        assert.strictEqual((await run(root, newerCrossEvent)).result.authorized, false,
            'a newer same-SHA run from another source event must suppress stale authorization');

        const changedPageCount = fixture();
        changedPageCount.normalizedPages = { workflow_runs: [
            { total_count: 2, items: [changedPageCount.run] },
            { total_count: 1, items: [{ ...clone(changedPageCount.run), id: RUN_ID + 1, run_number: 51 }] }
        ] };
        assert.strictEqual((await run(root, changedPageCount)).result.authorized, false,
            'Octokit-normalized total_count drift between pages must fail closed');

        const noProgress = fixture();
        noProgress.normalizedPages = { workflow_runs: [{ total_count: 1, items: [] }] };
        assert.strictEqual((await run(root, noProgress)).result.authorized, false,
            'Octokit-normalized pagination must reject an incomplete empty page');

        const linkedDirect = fixture();
        linkedDirect.useDirectListFallback = true;
        linkedDirect.runsNextLink = true;
        assert.strictEqual((await run(root, linkedDirect)).result.authorized, false,
            'direct single-page authorization must reject a next-page link');

        const mismatchedRunInventory = fixture();
        mismatchedRunInventory.runs[0].path = '.github/workflows/attacker.yml';
        assert.strictEqual((await run(root, mismatchedRunInventory)).result.authorized, false,
            'the selected inventory run must bind the exact trusted source workflow');

        const mismatchedJob = fixture();
        mismatchedJob.jobs[0].run_attempt = 2;
        assert.strictEqual((await run(root, mismatchedJob)).result.authorized, false,
            'the source job must bind the exact run attempt');

        const replayedArtifact = fixture();
        replayedArtifact.artifacts[0].created_at = '2026-08-01T01:00:30Z';
        replayedArtifact.artifacts[0].updated_at = '2026-08-01T01:00:31Z';
        assert.strictEqual((await run(root, replayedArtifact)).result.authorized, false,
            'an artifact created before the exact-attempt upload step must fail closed');

        const malformedArtifact = fixture();
        malformedArtifact.artifacts[0].node_id = '';
        assert.strictEqual((await run(root, malformedArtifact)).result.authorized, false,
            'artifact immutable identity fields must be non-empty and exact');

        const finalRuntime = harness(cleanData);
        const finalEventPath = writeEvent(root, cleanData.event);
        const finalValues = {
            REPORT_MODE: 'trusted-final',
            REPORTER_TEST_OUTCOME: 'success',
            TRUSTED_CHECKOUT_SHA: TRUSTED_SHA,
            TRUSTED_WORKFLOW_REF: 'Krilliac/SparkEngine/.github/workflows/codacy-report.yml@refs/heads/Working',
            TRUSTED_WORKFLOW_SHA: TRUSTED_SHA,
            WORKFLOW_RUN_EVENT_PATH: finalEventPath,
            PREFLIGHT_ARTIFACT_MANIFEST: clean.runtime.observed.outputs['artifact-manifest'],
            PREFLIGHT_UPLOAD_REF: 'refs/pull/42/head',
            PREFLIGHT_UPLOAD_SHA: SOURCE_SHA,
            DOWNLOAD_OUTCOME: 'success',
            SARIF_VALIDATION_OUTCOME: 'success'
        };
        const final = await withEnvironment(finalValues, () => authorize(finalRuntime));
        assert.strictEqual(final.authorized, true);

        const temporalRace = fixture();
        const newerAtFinalBoundary = {
            ...clone(temporalRace.run), id: RUN_ID + 200, run_number: 51
        };
        temporalRace.runInventoryResponses = [
            [clone(temporalRace.run)],
            [newerAtFinalBoundary, clone(temporalRace.run)]
        ];
        const temporalRuntime = harness(temporalRace);
        const temporalValues = {
            ...finalValues,
            WORKFLOW_RUN_EVENT_PATH: writeEvent(root, temporalRace.event)
        };
        const rejectedTemporalRace = await withEnvironment(
            temporalValues, () => authorize(temporalRuntime));
        assert.strictEqual(rejectedTemporalRace.authorized, false,
            'a newer same-SHA execution at the final authorization boundary must fail closed');
        assert.strictEqual(temporalRuntime.observed.outputs['authorized'], 'false');
        assert.strictEqual(temporalRuntime.observed.outputs['artifact-id'], '');
        assert.strictEqual(temporalRuntime.observed.outputs['upload-sha'], '');
        assert.strictEqual(temporalRuntime.observed.listRunRequests.length, 2,
            'final authorization must perform an immediate second same-SHA inventory');

        for (const [shape, useDirectListFallback] of [['normalized', false], ['direct', true]]) {
            for (const inventory of ['jobs', 'runs', 'artifacts']) {
                const truncated = fixture();
                truncated.useDirectListFallback = useDirectListFallback;
                truncated[`${inventory}TotalCount`] = truncated[inventory].length + 1;
                const runtime = harness(truncated);
                const values = {
                    ...finalValues,
                    WORKFLOW_RUN_EVENT_PATH: writeEvent(root, truncated.event)
                };
                const rejected = await withEnvironment(values, () => authorize(runtime));
                assert.strictEqual(rejected.authorized, false,
                    `${shape} truncated ${inventory} inventory must fail final authorization closed`);
                assert.strictEqual(runtime.observed.outputs['authorized'], 'false');
                assert.strictEqual(runtime.observed.outputs['artifact-id'], '');
                assert.strictEqual(runtime.observed.outputs['upload-sha'], '');
            }
        }

        const sourceDrift = fixture();
        sourceDrift.sourceWorkflowBlob = '7'.repeat(40);
        assert.strictEqual((await run(root, sourceDrift)).result.authorized, false);

        const reporterDrift = fixture();
        reporterDrift.defaultCommitSha = '8'.repeat(40);
        assert.strictEqual((await run(root, reporterDrift)).result.authorized, false);

        const artifactDrift = fixture();
        artifactDrift.artifacts[0].workflow_run.head_sha = '9'.repeat(40);
        assert.strictEqual((await run(root, artifactDrift)).result.authorized, false);

        const newerRun = fixture();
        newerRun.runs.push({ ...clone(newerRun.run), id: RUN_ID + 1, run_number: 51 });
        assert.strictEqual((await run(root, newerRun)).result.authorized, false);

        const changedPull = fixture();
        changedPull.pull.head.sha = 'a'.repeat(40);
        changedPull.pulls = [changedPull.pull];
        assert.strictEqual((await run(root, changedPull)).result.authorized, false);

        const push = fixture();
        push.run.event = 'push';
        push.run.path = '.github/workflows/codacy.yml@refs/heads/Working';
        push.run.head_branch = 'Working';
        push.run.repository = push.repository;
        push.run.head_repository = push.repository;
        push.run.pull_requests = [];
        push.event.workflow_run = clone(push.run);
        push.runs = [push.run];
        push.jobs[0].head_branch = 'Working';
        push.artifacts[0].workflow_run.head_repository_id = REPOSITORY_ID;
        push.artifacts[0].workflow_run.head_branch = 'Working';
        const pushResult = await run(root, push);
        assert.strictEqual(pushResult.result.authorized, true);
        assert.strictEqual(pushResult.runtime.observed.outputs['upload-ref'], 'refs/heads/Working');
        assert.strictEqual(pushResult.runtime.observed.outputs['upload-sha'], SOURCE_SHA);

        const changedManifestRuntime = harness(cleanData);
        const changedManifestPath = writeEvent(root, cleanData.event);
        const changedManifest = JSON.parse(clean.runtime.observed.outputs['artifact-manifest']);
        changedManifest.digest = `sha256:${'f'.repeat(64)}`;
        const rejectedFinal = await withEnvironment({
            REPORT_MODE: 'trusted-final',
            REPORTER_TEST_OUTCOME: 'success',
            TRUSTED_CHECKOUT_SHA: TRUSTED_SHA,
            TRUSTED_WORKFLOW_REF: 'Krilliac/SparkEngine/.github/workflows/codacy-report.yml@refs/heads/Working',
            TRUSTED_WORKFLOW_SHA: TRUSTED_SHA,
            WORKFLOW_RUN_EVENT_PATH: changedManifestPath,
            PREFLIGHT_ARTIFACT_MANIFEST: JSON.stringify(changedManifest),
            PREFLIGHT_UPLOAD_REF: 'refs/pull/42/head',
            PREFLIGHT_UPLOAD_SHA: SOURCE_SHA,
            DOWNLOAD_OUTCOME: 'success',
            SARIF_VALIDATION_OUTCOME: 'success'
        }, () => authorize(changedManifestRuntime));
        assert.strictEqual(rejectedFinal.authorized, false);

        console.log('authorize-codacy-sarif trusted scenarios passed');
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
