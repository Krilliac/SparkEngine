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
        name: 'results.sarif',
        size_in_bytes: 4096,
        expired: false,
        digest: DIGEST,
        workflow_run: {
            id: RUN_ID,
            repository_id: REPOSITORY_ID,
            head_repository_id: HEAD_REPOSITORY_ID,
            head_branch: run.head_branch,
            head_sha: SOURCE_SHA
        }
    };
    return {
        repository,
        run,
        event: { action: 'completed', repository, workflow_run: clone(run) },
        pull,
        pulls: [pull],
        artifacts: [artifact],
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
    const observed = { outputs: {}, failed: [], info: [], getContent: [] };
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
                async listWorkflowRuns() {
                    return { data: { workflow_runs: clone(state.runs) }, headers: {} };
                },
                async listWorkflowRunArtifacts() {
                    return { data: { artifacts: clone(state.artifacts) }, headers: {} };
                }
            },
            pulls: {
                async get() { return { data: clone(state.pull) }; },
                async list() { return { data: clone(state.pulls), headers: {} }; }
            }
        }
    };
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

        const finalRuntime = harness(cleanData);
        const finalEventPath = writeEvent(root, cleanData.event);
        const final = await withEnvironment({
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
        }, () => authorize(finalRuntime));
        assert.strictEqual(final.authorized, true);

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
