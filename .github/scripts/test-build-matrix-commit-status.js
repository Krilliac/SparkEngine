const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const buildMatrixStatus = require('./build-matrix-commit-status.js');

const SHA = '1111111111111111111111111111111111111111';
const OTHER_SHA = '2222222222222222222222222222222222222222';
const REPOSITORY_ID = 101;
const WORKFLOW_ID = 202;
const RUN_ID = 303;
const REPORTER_RUN_ID = 404;
const WORKFLOW_BLOB_SHA = '3333333333333333333333333333333333333333';
const RAW_DIGEST_A = 'a'.repeat(64);
const RAW_DIGEST_B = 'b'.repeat(64);
const RAW_DIGEST_C = 'c'.repeat(64);
const ENV_KEYS = [
    'BUILD_MATRIX_STATUS_MODE', 'TRUSTED_CHECKOUT_SHA', 'TRUSTED_WORKFLOW_REF',
    'TRUSTED_WORKFLOW_SHA', 'VERIFIER_TEST_OUTCOME', 'PREFLIGHT_OUTCOME',
    'DOWNLOAD_OUTCOME', 'VERIFY_EVIDENCE_OUTCOME', 'ATTEST_RECEIPT_OUTCOME',
    'UPLOAD_RECEIPT_OUTCOME', 'SOURCE_METADATA_PATH', 'RECEIPT_PATH',
    'STATUS_RECORD_PATH', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT'
];

const clone = value => JSON.parse(JSON.stringify(value));

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

function fixture(action = 'in_progress') {
    const repository = {
        id: REPOSITORY_ID,
        full_name: 'Krilliac/SparkEngine',
        default_branch: 'Working'
    };
    const run = {
        id: RUN_ID,
        workflow_id: WORKFLOW_ID,
        run_number: 50,
        run_attempt: 1,
        name: 'Build SparkEngine',
        path: '.github/workflows/build.yml@refs/heads/Working',
        event: 'push',
        status: action === 'in_progress' ? 'in_progress' : 'completed',
        conclusion: action === 'in_progress' ? null : 'success',
        head_branch: 'Working',
        head_sha: SHA,
        repository,
        head_repository: repository
    };
    const requiredSteps = [
        'Checkout repository',
        'Setup MSVC',
        'Configure and build the Windows Shipping lane',
        'Capture Windows Shipping provenance',
        'Install the Windows Shipping SDK for the consumer profile',
        'Configure and build the Windows validation lane',
        'Capture Windows validation provenance',
        'Configure and build the installed SDK consumer lane',
        'Capture installed SDK consumer provenance',
        'Generate the configured build-matrix inventory',
        'Check the configured build matrix',
        'Upload build-matrix evidence'
    ];
    const sourceJob = {
        id: 606,
        name: 'Windows Shipping build matrix',
        run_id: run.id,
        run_attempt: run.run_attempt,
        head_sha: run.head_sha,
        head_branch: run.head_branch,
        workflow_name: run.name,
        run_url: `https://api.github.com/repos/${repository.full_name}/actions/runs/${run.id}`,
        status: 'completed',
        conclusion: 'success',
        steps: [
            ...requiredSteps.map(name => ({ name, status: 'completed', conclusion: 'success' })),
            { name: 'Record build-matrix evidence', status: 'completed', conclusion: 'success' }
        ]
    };
    const sourceArtifact = {
        id: 707,
        name: `build-matrix-stable-v1-${SHA}-1`,
        expired: false,
        size_in_bytes: 8192,
        digest: `sha256:${'a'.repeat(64)}`,
        workflow_run: {
            id: run.id,
            repository_id: repository.id,
            head_repository_id: repository.id,
            head_branch: run.head_branch,
            head_sha: run.head_sha
        }
    };
    return {
        repository,
        run,
        event: { action, repository, workflow_run: clone(run) },
        workflowRuns: [run],
        defaultSha: SHA,
        resolvedSha: SHA,
        sourceWorkflowBlobSha: WORKFLOW_BLOB_SHA,
        trustedWorkflowBlobSha: WORKFLOW_BLOB_SHA,
        sourceJobs: [sourceJob],
        sourceArtifacts: [sourceArtifact]
    };
}

function harness(data) {
    const state = clone(data);
    const observed = {
        failed: [], info: [], outputs: {}, statuses: [], workflowRunCalls: 0,
        listRunRequests: [], commitRequests: [], contentRequests: []
    };
    const github = {
        rest: {
            repos: {
                async get() { return { data: clone(state.repository) }; },
                async getCommit(request) {
                    observed.commitRequests.push(clone(request));
                    const sha = request.ref === 'Working' ? state.defaultSha : state.resolvedSha;
                    return { data: { sha } };
                },
                async getContent(request) {
                    observed.contentRequests.push(clone(request));
                    const blobSha = request.ref === state.run.head_sha
                        ? state.sourceWorkflowBlobSha : state.trustedWorkflowBlobSha;
                    return { data: { type: 'file', sha: blobSha } };
                },
                async createCommitStatus(request) {
                    observed.statuses.push(clone(request));
                    if (state.statusApiError) throw new Error(state.statusApiError);
                    return { data: { id: 505 } };
                }
            },
            actions: {
                async getWorkflowRun() {
                    const index = observed.workflowRunCalls;
                    observed.workflowRunCalls += 1;
                    const responses = state.workflowRunResponses;
                    const value = Array.isArray(responses) && responses.length
                        ? responses[Math.min(index, responses.length - 1)] : state.run;
                    return { data: clone(value) };
                },
                async listWorkflowRuns(request) {
                    observed.listRunRequests.push(clone(request));
                    const responseIndex = observed.listRunRequests.length - 1;
                    const inventories = state.workflowRunInventoryResponses;
                    const workflowRuns = Array.isArray(inventories) && inventories.length
                        ? inventories[Math.min(responseIndex, inventories.length - 1)]
                        : state.workflowRuns;
                    return {
                        data: {
                            total_count: state.workflowRunsTotalCount ?? workflowRuns.length,
                            workflow_runs: clone(workflowRuns)
                        },
                        headers: {}
                    };
                },
                async listJobsForWorkflowRunAttempt() {
                    return {
                        data: {
                            total_count: state.jobsTotalCount ?? state.sourceJobs.length,
                            jobs: clone(state.sourceJobs)
                        }
                    };
                },
                async listWorkflowRunArtifacts() {
                    return {
                        data: {
                            total_count: state.artifactsTotalCount ?? state.sourceArtifacts.length,
                            artifacts: clone(state.sourceArtifacts)
                        }
                    };
                }
            }
        },
        paginate: {
            async *iterator(method, request) {
                const response = await method(request);
                const normalized = clone(response.data.workflow_runs);
                normalized.total_count = response.data.total_count;
                yield { ...response, data: normalized };
            }
        }
    };
    if (state.useDirectListFallback) delete github.paginate;
    const core = {
        setFailed: message => observed.failed.push(message),
        info: message => observed.info.push(message),
        setOutput: (name, value) => { observed.outputs[name] = String(value); }
    };
    const context = {
        repo: { owner: 'Krilliac', repo: 'SparkEngine' },
        payload: clone(state.event)
    };
    return { github, core, context, observed };
}

function baseEnvironment(mode) {
    return {
        BUILD_MATRIX_STATUS_MODE: mode,
        TRUSTED_CHECKOUT_SHA: SHA,
        TRUSTED_WORKFLOW_SHA: SHA,
        TRUSTED_WORKFLOW_REF: 'Krilliac/SparkEngine/.github/workflows/build-matrix-verifier.yml@refs/heads/Working',
        GITHUB_RUN_ID: String(REPORTER_RUN_ID),
        GITHUB_RUN_ATTEMPT: '2'
    };
}

async function runMode(data, mode, overrides = {}) {
    const runtime = harness(data);
    const result = await withEnvironment(
        { ...baseEnvironment(mode), ...overrides },
        () => buildMatrixStatus(runtime)
    );
    return { runtime, result };
}

function sourceMetadata() {
    return {
        schemaVersion: 1,
        repository: { id: REPOSITORY_ID, fullName: 'Krilliac/SparkEngine', defaultBranch: 'Working' },
        source: {
            workflowId: WORKFLOW_ID,
            workflowName: 'Build SparkEngine',
            workflowPath: '.github/workflows/build.yml',
            runId: RUN_ID,
            runNumber: 50,
            runAttempt: 1,
            event: 'push',
            conclusion: 'success',
            headBranch: 'Working',
            headSha: SHA,
            jobId: 606,
            jobName: 'Windows Shipping build matrix',
            jobConclusion: 'success',
            finalStepName: 'Record build-matrix evidence',
            finalStepConclusion: 'success'
        },
        artifact: {
            id: 707,
            name: `build-matrix-stable-v1-${SHA}-1`,
            bytes: 8192,
            digest: `sha256:${'a'.repeat(64)}`
        },
        verifier: {
            repository: 'Krilliac/SparkEngine',
            checkoutSha: SHA,
            workflowSha: SHA,
            workflowRef: 'Krilliac/SparkEngine/.github/workflows/build-matrix-verifier.yml@refs/heads/Working',
            sourceWorkflowBlobSha: WORKFLOW_BLOB_SHA,
            trustedWorkflowBlobSha: WORKFLOW_BLOB_SHA
        }
    };
}

function trustedReceipt(metadata = sourceMetadata()) {
    return {
        schemaVersion: 1,
        kind: 'spark-build-matrix-trusted-workflow-run',
        state: 'verified',
        authority: 'github-actions-protected-workflow-run-v1',
        profile: 'stable-v1',
        source: {
            repository: metadata.repository.fullName,
            workflowId: metadata.source.workflowId,
            workflowName: metadata.source.workflowName,
            workflowPath: metadata.source.workflowPath,
            runId: metadata.source.runId,
            runNumber: metadata.source.runNumber,
            runAttempt: metadata.source.runAttempt,
            event: metadata.source.event,
            conclusion: metadata.source.conclusion,
            headBranch: metadata.source.headBranch,
            headSha: metadata.source.headSha,
            jobId: metadata.source.jobId,
            jobName: metadata.source.jobName,
            jobConclusion: metadata.source.jobConclusion,
            expectedFinalStep: metadata.source.finalStepName
        },
        verifier: {
            repository: metadata.verifier.repository,
            workflowRef: metadata.verifier.workflowRef,
            workflowSha: metadata.verifier.workflowSha,
            checkoutSha: metadata.verifier.checkoutSha,
            sourceWorkflowBlobSha: metadata.verifier.sourceWorkflowBlobSha,
            trustedWorkflowBlobSha: metadata.verifier.trustedWorkflowBlobSha
        },
        inputArtifact: {
            id: metadata.artifact.id,
            name: metadata.artifact.name,
            bytes: metadata.artifact.bytes,
            digest: metadata.artifact.digest,
            extractedFileCount: 42,
            extractedBytes: 16384,
            inventorySha256: RAW_DIGEST_B,
            parityReportSha256: RAW_DIGEST_C
        },
        profiles: [
            'installed-sdk-consumer',
            'windows-shipping',
            'windows-validation'
        ].map((id, index) => ({
            id,
            recordSha256: index === 0 ? RAW_DIGEST_A : index === 1 ? RAW_DIGEST_B : RAW_DIGEST_C,
            replyDigest: index === 0 ? RAW_DIGEST_B : index === 1 ? RAW_DIGEST_C : RAW_DIGEST_A,
            artifactManifestSha256: index === 0 ? RAW_DIGEST_C : index === 1 ? RAW_DIGEST_A : RAW_DIGEST_B,
            targetCount: 1,
            artifactCount: 1
        })),
        pendingState: {
            state: 'pending-external-attestation',
            authority: 'external-attestation-required',
            errorCount: 3,
            warningCount: 2
        }
    };
}

function testPreflightApiGuards() {
    const exactSinglePageInventory = buildMatrixStatus._test.exactSinglePageInventory ||
        ((response, key) => response.data[key]);
    const completeJobs = { data: { total_count: 1, jobs: [{ id: 1 }] } };
    assert.deepStrictEqual(
        exactSinglePageInventory(completeJobs, 'jobs', 'Source job inventory'),
        completeJobs.data.jobs
    );
    assert.throws(
        () => exactSinglePageInventory(
            { data: { total_count: 2, jobs: [{ id: 1 }] } },
            'jobs',
            'Source job inventory'
        ),
        /incomplete or malformed/,
        'a truncated single-page job inventory must fail closed'
    );
    assert.throws(
        () => exactSinglePageInventory(
            { data: { total_count: 2, artifacts: [{ id: 1 }] } },
            'artifacts',
            'Source artifact inventory'
        ),
        /incomplete or malformed/,
        'a truncated single-page artifact inventory must fail closed'
    );
    const overBound = Array.from({ length: 101 }, (_, id) => ({ id: id + 1 }));
    assert.throws(
        () => exactSinglePageInventory(
            { data: { total_count: overBound.length, artifacts: overBound } },
            'artifacts',
            'Source artifact inventory'
        ),
        /incomplete or malformed/,
        'an otherwise complete single-page inventory above 100 entries must fail closed'
    );

    const artifactWorkflowRunMatches = buildMatrixStatus._test.artifactWorkflowRunMatches || (() => true);
    const data = fixture('completed');
    const artifact = {
        workflow_run: {
            id: data.run.id,
            repository_id: data.repository.id,
            head_repository_id: data.run.head_repository.id,
            head_branch: data.run.head_branch,
            head_sha: data.run.head_sha
        }
    };
    assert.strictEqual(artifactWorkflowRunMatches(artifact, data.run, data.repository), true);
    const provenanceMutations = [
        ['source run ID', value => { value.workflow_run.id += 1; }],
        ['repository ID', value => { value.workflow_run.repository_id += 1; }],
        ['head repository ID', value => { value.workflow_run.head_repository_id += 1; }],
        ['head branch', value => { value.workflow_run.head_branch = 'forged'; }],
        ['head SHA', value => { value.workflow_run.head_sha = OTHER_SHA; }]
    ];
    for (const [label, mutate] of provenanceMutations) {
        const changed = clone(artifact);
        mutate(changed);
        assert.strictEqual(
            artifactWorkflowRunMatches(changed, data.run, data.repository),
            false,
            `artifact provenance must reject a changed ${label}`
        );
    }
}

async function testSourceJobArtifactAuthorization() {
    assert.strictEqual(typeof buildMatrixStatus.authorizeSourceJobArtifact, 'function',
        'the workflow must call one tested source job/artifact authorizer');
    const data = fixture('completed');
    const runtime = harness(data);
    const authorized = await buildMatrixStatus.authorizeSourceJobArtifact({
        github: runtime.github,
        context: runtime.context,
        run: data.run,
        repository: data.repository,
        sourceSha: SHA
    });
    assert.strictEqual(authorized.sourceJob.id, 606);
    assert.strictEqual(authorized.artifact.id, 707);
    assert.strictEqual(authorized.finalStep.conclusion, 'success');

    const hostileJobs = [
        ['null inventory entry', value => { value.sourceJobs.push(null); }],
        ['non-object inventory entry', value => { value.sourceJobs.push('not-a-job'); }],
        ['duplicate job ID', value => {
            const other = clone(value.sourceJobs[0]);
            other.name = 'Other exact-attempt job';
            other.conclusion = 'success';
            other.steps = [];
            value.sourceJobs.push(other);
        }],
        ['nonpositive job ID', value => {
            const other = clone(value.sourceJobs[0]);
            other.id = 0;
            other.name = 'Other exact-attempt job';
            other.conclusion = 'success';
            other.steps = [];
            value.sourceJobs.push(other);
        }],
        ['empty job name', value => {
            const other = clone(value.sourceJobs[0]);
            other.id += 1;
            other.name = '';
            other.conclusion = 'success';
            other.steps = [];
            value.sourceJobs.push(other);
        }],
        ['producer run ID', value => { value.sourceJobs[0].run_id += 1; }],
        ['producer run attempt', value => { value.sourceJobs[0].run_attempt += 1; }],
        ['producer head SHA', value => { value.sourceJobs[0].head_sha = OTHER_SHA; }],
        ['producer head branch', value => { value.sourceJobs[0].head_branch = 'forged'; }],
        ['producer workflow name', value => { value.sourceJobs[0].workflow_name = 'Other workflow'; }],
        ['producer run URL', value => { value.sourceJobs[0].run_url = 'https://api.github.com/forged'; }]
    ];
    const hostileFailures = [];
    for (const [label, mutate] of hostileJobs) {
        const hostile = fixture('completed');
        mutate(hostile);
        const hostileRuntime = harness(hostile);
        try {
            await buildMatrixStatus.authorizeSourceJobArtifact({
                github: hostileRuntime.github,
                context: hostileRuntime.context,
                run: hostile.run,
                repository: hostile.repository,
                sourceSha: SHA
            });
            hostileFailures.push(`${label}: accepted`);
        } catch (error) {
            if (!/job inventory|job provenance/.test(error.message)) {
                hostileFailures.push(`${label}: unexpected rejection '${error.message}'`);
            }
        }
    }
    assert.deepStrictEqual(hostileFailures, [],
        `source job authorization must fail closed:\n${hostileFailures.join('\n')}`);

    const truncated = fixture('completed');
    truncated.jobsTotalCount = 2;
    await assert.rejects(
        () => buildMatrixStatus.authorizeSourceJobArtifact({
            github: harness(truncated).github,
            context: runtime.context,
            run: truncated.run,
            repository: truncated.repository,
            sourceSha: SHA
        }),
        /incomplete or malformed/
    );

    const forged = fixture('completed');
    forged.sourceArtifacts[0].workflow_run.head_sha = OTHER_SHA;
    await assert.rejects(
        () => buildMatrixStatus.authorizeSourceJobArtifact({
            github: harness(forged).github,
            context: runtime.context,
            run: forged.run,
            repository: forged.repository,
            sourceSha: SHA
        }),
        /not tied to the exact source run/
    );
}

function finalEnvironment(root, metadata = sourceMetadata(), receipt = trustedReceipt(metadata)) {
    const metadataPath = path.join(root, `metadata-${Math.random().toString(16).slice(2)}.json`);
    const receiptPath = path.join(root, `receipt-${Math.random().toString(16).slice(2)}.json`);
    const recordPath = path.join(root, `record-${Math.random().toString(16).slice(2)}.json`);
    fs.writeFileSync(metadataPath, JSON.stringify(metadata), 'utf8');
    fs.writeFileSync(receiptPath, JSON.stringify(receipt), 'utf8');
    return {
        VERIFIER_TEST_OUTCOME: 'success',
        PREFLIGHT_OUTCOME: 'success',
        DOWNLOAD_OUTCOME: 'success',
        VERIFY_EVIDENCE_OUTCOME: 'success',
        ATTEST_RECEIPT_OUTCOME: 'success',
        UPLOAD_RECEIPT_OUTCOME: 'success',
        SOURCE_METADATA_PATH: metadataPath,
        RECEIPT_PATH: receiptPath,
        STATUS_RECORD_PATH: recordPath
    };
}

function testWorkflowShape() {
    const workflow = fs.readFileSync(path.join(__dirname, '..', 'workflows', 'build-matrix-verifier.yml'), 'utf8');
    const build = fs.readFileSync(path.join(__dirname, '..', 'workflows', 'build.yml'), 'utf8');
    assert(workflow.includes('types: [in_progress, completed]'));
    assert(workflow.includes('statuses: write'));
    assert(!workflow.includes('checks: write'));
    assert(workflow.includes('group: build-matrix-trusted-source-${{ github.event.workflow_run.head_sha }}'));
    assert.strictEqual((workflow.match(/^\s+queue:/gm) || []).length, 0,
        'concurrency blocks must use only supported GitHub Actions keys');
    assert(workflow.includes("github.event.action == 'in_progress'"));
    assert(workflow.includes("github.event.action == 'completed'"));
    assert(workflow.includes('BUILD_MATRIX_STATUS_MODE: pending'));
    assert(workflow.includes('BUILD_MATRIX_STATUS_MODE: final'));
    assert(workflow.includes('status.authorizeSourceJobArtifact({'),
        'the trusted workflow must use the behavior-tested source job/artifact authorizer');
    assert(workflow.includes('id: upload-receipt'));
    assert(workflow.includes('build-matrix-trusted-receipt-${{ github.event.workflow_run.head_sha }}-${{ github.event.workflow_run.id }}-${{ github.event.workflow_run.run_attempt }}-${{ github.run_attempt }}'),
        'the durable receipt artifact must bind the exact source run and attempt');
    assert(workflow.includes("if: always() && steps.trusted-attestation.outcome == 'success'"));
    assert(build.includes('node .github/scripts/test-build-matrix-commit-status.js'),
        'the trusted status tests must run in validate-ci-tools');
}

async function main() {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'spark-build-matrix-status-'));
    try {
        testWorkflowShape();
        testPreflightApiGuards();
        await testSourceJobArtifactAuthorization();

        await assert.rejects(
            () => withEnvironment({ BUILD_MATRIX_STATUS_MODE: 'unsafe' }, () => buildMatrixStatus(harness(fixture()))),
            /Unsupported BUILD_MATRIX_STATUS_MODE/
        );

        const pending = await runMode(fixture(), 'pending');
        assert.strictEqual(pending.runtime.observed.failed.length, 0);
        assert.strictEqual(pending.runtime.observed.statuses.length, 2);
        assert.deepStrictEqual(pending.runtime.observed.statuses[0], {
            owner: 'Krilliac',
            repo: 'SparkEngine',
            sha: SHA,
            state: 'pending',
            target_url: `https://github.com/Krilliac/SparkEngine/actions/runs/${REPORTER_RUN_ID}/attempts/2`,
            description: `Trusted build-matrix verification running for Build run ${RUN_ID}, attempt 1.`,
            context: 'Build Matrix Verifier / Exact Source'
        });
        assert.deepStrictEqual(pending.runtime.observed.statuses[1], {
            owner: 'Krilliac',
            repo: 'SparkEngine',
            sha: SHA,
            state: 'pending',
            target_url: `https://github.com/Krilliac/SparkEngine/actions/runs/${REPORTER_RUN_ID}/attempts/2`,
            description: `Trusted exact-source aggregate is awaiting both reporters for ${SHA.slice(0, 12)}.`,
            context: 'Trusted Exact-Source CI / Aggregate'
        });
        assert.strictEqual(pending.runtime.observed.listRunRequests[0].event, undefined,
            'same-SHA suppression must span push and workflow_dispatch events');

        for (const [shape, useDirectListFallback] of [['normalized', false], ['direct', true]]) {
            for (const mode of ['pending', 'final']) {
                const truncatedData = fixture(mode === 'final' ? 'completed' : 'in_progress');
                truncatedData.workflowRunsTotalCount = 2;
                truncatedData.useDirectListFallback = useDirectListFallback;
                const overrides = mode === 'final' ? finalEnvironment(root) : {};
                const truncated = await runMode(truncatedData, mode, overrides);
                assert.strictEqual(truncated.runtime.observed.statuses.length, 0,
                    `${shape} truncated workflow-run inventory must not publish ${mode} status`);
                assert(truncated.runtime.observed.failed.some(message =>
                    message.includes('incomplete') || message.includes('total_count')),
                `${shape} truncated workflow-run inventory must fail closed before ${mode} status publication`);
            }
        }

        for (const mode of ['pending', 'final']) {
            const duplicateExecution = fixture(mode === 'final' ? 'completed' : 'in_progress');
            const conflictingRun = clone(duplicateExecution.run);
            conflictingRun.id += 100;
            duplicateExecution.workflowRuns = [conflictingRun, duplicateExecution.run];
            const result = await runMode(
                duplicateExecution, mode, mode === 'final' ? finalEnvironment(root) : {});
            assert.strictEqual(result.runtime.observed.statuses.length, 0,
                `distinct run IDs reusing one execution identity must not publish ${mode} status`);
            assert(result.runtime.observed.failed.some(message => message.includes('malformed')),
                `duplicate execution identity must fail ${mode} closed`);
        }

        for (const mode of ['pending', 'final']) {
            const temporalRace = fixture(mode === 'final' ? 'completed' : 'in_progress');
            const newer = clone(temporalRace.run);
            newer.id += 200;
            newer.run_number += 1;
            temporalRace.workflowRunInventoryResponses = [
                [clone(temporalRace.run)], [newer, clone(temporalRace.run)]
            ];
            const raced = await runMode(
                temporalRace, mode, mode === 'final' ? finalEnvironment(root) : {});
            assert.strictEqual(raced.runtime.observed.statuses.length, 0,
                `a newer execution appearing in the immediate inventory must block ${mode} status`);
            assert(raced.runtime.observed.failed.some(message => message.includes('newer build-matrix source run')),
                `the immediate ${mode} inventory race must fail closed`);
        }

        const newerDispatchData = fixture();
        const newerDispatch = clone(newerDispatchData.run);
        newerDispatch.id += 1;
        newerDispatch.run_number += 1;
        newerDispatch.event = 'workflow_dispatch';
        newerDispatchData.workflowRuns = [newerDispatch, newerDispatchData.run];
        const superseded = await runMode(newerDispatchData, 'pending');
        assert.strictEqual(superseded.runtime.observed.statuses.length, 0);
        assert(superseded.runtime.observed.failed.some(message => message.includes('newer build-matrix source run')));

        const lateData = fixture();
        const completed = clone(lateData.run);
        completed.status = 'completed';
        completed.conclusion = 'success';
        lateData.workflowRunResponses = [lateData.run, completed];
        const late = await runMode(lateData, 'pending');
        assert.strictEqual(late.runtime.observed.statuses.length, 0,
            'a late in-progress event must not overwrite a completed verifier result');

        const staleDefaultData = fixture();
        staleDefaultData.defaultSha = OTHER_SHA;
        const staleDefault = await runMode(staleDefaultData, 'pending');
        assert.strictEqual(staleDefault.runtime.observed.statuses.length, 0);
        assert(staleDefault.runtime.observed.failed.some(message => message.includes('not exact')));

        const historicalData = fixture();
        historicalData.defaultSha = OTHER_SHA;
        const historical = await runMode(historicalData, 'pending', {
            TRUSTED_CHECKOUT_SHA: OTHER_SHA,
            TRUSTED_WORKFLOW_SHA: OTHER_SHA
        });
        assert.strictEqual(historical.runtime.observed.failed.length, 0,
            'a historical source remains eligible when the current trusted verifier uses the same Build workflow');
        assert.strictEqual(historical.runtime.observed.statuses.length, 2);

        const changedWorkflowData = fixture();
        changedWorkflowData.defaultSha = OTHER_SHA;
        changedWorkflowData.trustedWorkflowBlobSha = OTHER_SHA;
        const changedWorkflow = await runMode(changedWorkflowData, 'pending', {
            TRUSTED_CHECKOUT_SHA: OTHER_SHA,
            TRUSTED_WORKFLOW_SHA: OTHER_SHA
        });
        assert.strictEqual(changedWorkflow.runtime.observed.statuses.length, 0);
        assert(changedWorkflow.runtime.observed.failed.some(message => message.includes('byte-identical')));

        const successEnv = finalEnvironment(root);
        const success = await runMode(fixture('completed'), 'final', successEnv);
        assert.strictEqual(success.runtime.observed.failed.length, 0);
        assert.strictEqual(success.runtime.observed.statuses.length, 3);
        assert.deepStrictEqual(success.runtime.observed.statuses[0], {
            owner: 'Krilliac',
            repo: 'SparkEngine',
            sha: SHA,
            state: 'pending',
            target_url: `https://github.com/Krilliac/SparkEngine/actions/runs/${REPORTER_RUN_ID}/attempts/2`,
            description: `Trusted build-matrix verification running for Build run ${RUN_ID}, attempt 1.`,
            context: 'Build Matrix Verifier / Exact Source'
        });
        assert.strictEqual(success.runtime.observed.statuses[1].context,
            'Trusted Exact-Source CI / Aggregate');
        assert.strictEqual(success.runtime.observed.statuses[2].state, 'success');
        assert.strictEqual(success.runtime.observed.statuses[2].sha, SHA);
        assert.strictEqual(success.runtime.observed.statuses[2].description,
            `Trusted build-matrix verified for Build run ${RUN_ID}, attempt 1.`);
        assert.strictEqual(success.result.evidenceState, 'verified');
        assert(fs.existsSync(successEnv.STATUS_RECORD_PATH));
        const successRecord = JSON.parse(fs.readFileSync(successEnv.STATUS_RECORD_PATH, 'utf8'));
        assert.strictEqual(successRecord.commitStatus.context, 'Build Matrix Verifier / Exact Source');

        const failedOutcomeEnv = {
            ...finalEnvironment(root),
            DOWNLOAD_OUTCOME: 'failure'
        };
        const failedOutcome = await runMode(fixture('completed'), 'final', failedOutcomeEnv);
        assert.strictEqual(failedOutcome.runtime.observed.statuses[0].state, 'pending');
        assert.strictEqual(failedOutcome.runtime.observed.statuses[1].context,
            'Trusted Exact-Source CI / Aggregate');
        assert.strictEqual(failedOutcome.runtime.observed.statuses[2].state, 'failure');
        assert.strictEqual(failedOutcome.result.evidenceState, 'incomplete');
        assert(failedOutcome.runtime.observed.failed.some(message => message.includes('artifact download')));

        const badReceiptMetadata = sourceMetadata();
        const badReceipt = trustedReceipt(badReceiptMetadata);
        badReceipt.source.headSha = OTHER_SHA;
        const badReceiptResult = await runMode(
            fixture('completed'), 'final', finalEnvironment(root, badReceiptMetadata, badReceipt));
        assert.strictEqual(badReceiptResult.runtime.observed.statuses[0].state, 'pending');
        assert.strictEqual(badReceiptResult.runtime.observed.statuses[2].state, 'failure');
        assert(badReceiptResult.result.evidenceErrors.some(error => error.includes('does not bind')));

        const receiptMutations = [
            ['empty profiles', receipt => { receipt.profiles = []; }],
            ['wrong profile ID', receipt => { receipt.profiles[0].id = 'forged-profile'; }],
            ['empty profile digest', receipt => { receipt.profiles[0].recordSha256 = ''; }],
            ['negative profile target count', receipt => { receipt.profiles[0].targetCount = -1; }],
            ['wrong pending state', receipt => { receipt.pendingState.state = 'verified'; }],
            ['wrong pending authority', receipt => { receipt.pendingState.authority = 'forged'; }],
            ['wrong pending error count', receipt => { receipt.pendingState.errorCount = 2; }],
            ['wrong pending warning count', receipt => { receipt.pendingState.warningCount = 0; }],
            ['missing inventory digest', receipt => { delete receipt.inputArtifact.inventorySha256; }],
            ['malformed parity digest', receipt => { receipt.inputArtifact.parityReportSha256 = 'f'.repeat(63); }],
            ['zero extracted file count', receipt => { receipt.inputArtifact.extractedFileCount = 0; }],
            ['zero extracted byte count', receipt => { receipt.inputArtifact.extractedBytes = 0; }]
        ];
        for (const [label, mutate] of receiptMutations) {
            const metadata = sourceMetadata();
            const receipt = trustedReceipt(metadata);
            mutate(receipt);
            const rejected = await runMode(
                fixture('completed'), 'final', finalEnvironment(root, metadata, receipt));
            assert.strictEqual(
                rejected.runtime.observed.statuses[2].state,
                'failure',
                `final success must reject receipt mutation: ${label}`
            );
            assert(rejected.result.evidenceErrors.some(error => error.includes('semantic closure')),
                `receipt mutation must report semantic closure failure: ${label}`);
        }

        const apiErrorData = fixture('completed');
        apiErrorData.statusApiError = 'simulated status outage';
        const apiError = await runMode(apiErrorData, 'final', finalEnvironment(root));
        assert.strictEqual(apiError.result.commitStatus.reason, 'api-error');
        assert.strictEqual(apiError.runtime.observed.statuses.length, 1);
        assert.strictEqual(apiError.runtime.observed.statuses[0].context,
            'Build Matrix Verifier / Exact Source');
        assert(apiError.runtime.observed.failed.some(message => message.includes('status API failed')));

        console.log('build-matrix exact-source commit status scenarios passed');
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
