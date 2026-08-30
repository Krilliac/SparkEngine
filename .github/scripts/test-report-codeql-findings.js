const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const reportCodeqlFindings = require('./report-codeql-findings.js');

const ENV_KEYS = [
    'REPORT_MODE', 'ARTIFACT_DIR', 'EXPECTED_LANGUAGES', 'REPORTER_TEST_OUTCOME',
    'PREFLIGHT_OUTCOME', 'PREFLIGHT_ARTIFACT_MANIFEST', 'DOWNLOAD_OUTCOME',
    'SUMMARY_PATH', 'WORKFLOW_RUN_EVENT_PATH', 'GITHUB_EVENT_PATH'
];
const EXPECTED_LANGUAGES = ['actions', 'c-cpp', 'python'];
const ARTIFACT_FILES = { actions: 'actions.sarif', 'c-cpp': 'cpp.sarif', python: 'python.sarif' };
const DIAGNOSTIC_DESCRIPTORS = {
    actions: ['actions/diagnostics/successfully-extracted-files'],
    'c-cpp': [
        'cpp/diagnostics/successfully-extracted-files',
        'cpp/diagnostics/extraction-warnings',
        'cpp/diagnostics/failed-extractor-invocations'
    ],
    python: [
        'py/diagnostics/successfully-extracted-files',
        'py/diagnostics/extraction-warnings'
    ]
};
const REPOSITORY_ID = 1001;
const HEAD_REPOSITORY_ID = 2002;
const RUN_ID = 3003;
const WORKFLOW_ID = 4004;
const MERGE_SHA = '1111111111111111111111111111111111111111';
const SOURCE_HEAD_SHA = '2222222222222222222222222222222222222222';
const WORKFLOW_BLOB_SHA = '3333333333333333333333333333333333333333';
const CHANGED_SHA = '4444444444444444444444444444444444444444';
const MARKER = '<!-- spark-codeql-report -->';

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

function fixture() {
    const repository = {
        id: REPOSITORY_ID,
        full_name: 'Krilliac/SparkEngine',
        default_branch: 'Working'
    };
    const headRepository = {
        id: HEAD_REPOSITORY_ID,
        full_name: 'contributor/SparkEngine'
    };
    const pullReference = {
        number: 42,
        head: { sha: SOURCE_HEAD_SHA },
        base: { repo: repository }
    };
    const run = {
        id: RUN_ID,
        workflow_id: WORKFLOW_ID,
        run_number: 50,
        run_attempt: 1,
        name: 'CodeQL Advanced',
        path: '.github/workflows/codeql.yml@refs/pull/42/merge',
        event: 'pull_request',
        status: 'completed',
        conclusion: 'success',
        head_sha: SOURCE_HEAD_SHA,
        head_branch: 'hostile-sarif',
        repository,
        head_repository: headRepository,
        pull_requests: [pullReference]
    };
    const event = {
        action: 'completed',
        repository,
        workflow_run: clone(run)
    };
    const artifacts = EXPECTED_LANGUAGES.map((language, index) => ({
        id: 5000 + index,
        name: ARTIFACT_FILES[language],
        size_in_bytes: 1024 + index,
        expired: false,
        digest: `sha256:${String(index + 6).repeat(64)}`,
        workflow_run: {
            id: RUN_ID,
            repository_id: REPOSITORY_ID,
            head_repository_id: HEAD_REPOSITORY_ID,
            head_branch: run.head_branch,
            head_sha: SOURCE_HEAD_SHA
        }
    }));
    const pull = {
        number: 42,
        state: 'open',
        merge_commit_sha: MERGE_SHA,
        base: { repo: repository },
        head: { sha: SOURCE_HEAD_SHA, repo: headRepository }
    };
    return {
        repository,
        run,
        event,
        artifacts,
        pull,
        pullCandidates: [pull],
        workflowRuns: [run],
        comments: [],
        sourceWorkflowBlobSha: WORKFLOW_BLOB_SHA,
        trustedWorkflowBlobSha: WORKFLOW_BLOB_SHA
    };
}

function writeEvent(root, event) {
    const eventPath = path.join(root, `event-${Math.random().toString(16).slice(2)}.json`);
    fs.writeFileSync(eventPath, JSON.stringify(event), 'utf8');
    return eventPath;
}

function harness(data) {
    const state = clone(data);
    const observed = {
        failed: [], info: [], warnings: [], outputs: {}, jobSummary: '',
        createdComments: [], updatedComments: [], listCommentsCalls: 0,
        workflowRunCalls: 0, artifactListCalls: 0,
        getContentCalls: [], pullGetCalls: [], pullListCalls: [], commentRequests: [],
        normalizedPaginateFields: []
    };
    const github = {
        rest: {
            repos: {
                async get() { return { data: clone(state.repository) }; },
                async getContent(request) {
                    observed.getContentCalls.push(clone(request));
                    const sha = request.ref === state.repository.default_branch
                        ? state.trustedWorkflowBlobSha : state.sourceWorkflowBlobSha;
                    return { data: { type: 'file', sha } };
                },
                async getCommit() {
                    return { data: { parents: [{ sha: CHANGED_SHA }, { sha: SOURCE_HEAD_SHA }] } };
                }
            },
            actions: {
                async getWorkflowRun() {
                    observed.workflowRunCalls += 1;
                    return { data: clone(state.run) };
                },
                async listWorkflowRunArtifacts(request) {
                    observed.artifactListCalls += 1;
                    return {
                        data: state.directArtifactRootArray ? clone(state.artifacts) :
                            { total_count: state.artifacts.length, artifacts: clone(state.artifacts) },
                        headers: {}
                    };
                },
                async listWorkflowRuns() {
                    return {
                        data: state.directWorkflowRunsRootArray ? clone(state.workflowRuns) :
                            { total_count: state.workflowRuns.length, workflow_runs: clone(state.workflowRuns) },
                        headers: {}
                    };
                }
            },
            pulls: {
                async get(request) {
                    observed.pullGetCalls.push(clone(request));
                    return { data: clone(state.pull) };
                },
                async list(request) {
                    observed.pullListCalls.push(clone(request));
                    return { data: clone(state.pullCandidates), headers: {} };
                }
            },
            issues: {
                async listComments(request) {
                    observed.listCommentsCalls += 1;
                    observed.commentRequests.push(clone(request));
                    if (!state.commentPages) return { data: clone(state.comments), headers: {} };
                    const finalPage = state.commentLastPage || Math.max(...Object.keys(state.commentPages).map(Number));
                    const page = request.page || 1;
                    const link = finalPage > 1
                        ? `<https://api.github.test/comments?page=${finalPage}>; rel="last"` : '';
                    return { data: clone(state.commentPages[page] || []), headers: { link } };
                },
                async createComment(request) {
                    observed.createdComments.push(request);
                    return { data: { id: 9001 } };
                },
                async updateComment(request) {
                    observed.updatedComments.push(request);
                    return { data: { id: request.comment_id } };
                }
            }
        }
    };
    if (state.useNormalizedPaginateIterator) {
        github.paginate = {
            async *iterator(method, request) {
                const response = await method(request);
                if (Array.isArray(response.data)) {
                    yield response;
                    return;
                }
                const field = Object.keys(response.data || {})
                    .find(key => Array.isArray(response.data[key]));
                if (field) observed.normalizedPaginateFields.push(field);
                const override = field && state.normalizedPaginatePageOverrides &&
                    Object.hasOwn(state.normalizedPaginatePageOverrides, field)
                    ? state.normalizedPaginatePageOverrides[field]
                    : field ? response.data[field] : undefined;
                const normalizedPage = field ? clone(override) : undefined;
                if (Array.isArray(normalizedPage) && state.includeNormalizedPaginationMetadata) {
                    normalizedPage.total_count = normalizedPage.length;
                    normalizedPage.total_commits = undefined;
                }
                yield field ? { ...response, data: normalizedPage } : response;
            }
        };
    }
    const summary = {
        addRaw(body) { observed.jobSummary = body; return this; },
        async write() {}
    };
    const core = {
        info: message => observed.info.push(message),
        warning: message => observed.warnings.push(message),
        setFailed: message => observed.failed.push(message),
        setOutput: (name, value) => { observed.outputs[name] = String(value); },
        summary
    };
    const context = { repo: { owner: 'Krilliac', repo: 'SparkEngine' } };
    return { observed, github, core, context };
}

function sarif(language, results = [], automationLanguage = language, runOverrides = {}) {
    return {
        version: '2.1.0',
        runs: [{
            // CodeQL currently serializes the category with a trailing slash.
            // The reporter also accepts the historical no-slash form.
            automationDetails: { id: `/language:${automationLanguage}/` },
            tool: {
                driver: {
                    name: 'CodeQL',
                    notifications: DIAGNOSTIC_DESCRIPTORS[language].map(id => ({ id })),
                    rules: [{
                        id: 'cpp/test-finding',
                        name: 'TestFinding',
                        shortDescription: { text: 'Test finding' },
                        defaultConfiguration: { level: 'warning' }
                    }]
                }
            },
            results,
            invocations: [{
                executionSuccessful: true,
                toolExecutionNotifications: [],
                configurationNotifications: []
            }],
            ...runOverrides
        }]
    };
}

function finding(overrides = {}) {
    return {
        ruleId: 'cpp/test-finding',
        level: 'warning',
        message: { text: 'Untrusted data reaches a sensitive operation.' },
        locations: [{
            physicalLocation: {
                artifactLocation: { uri: 'SparkEngine/Source/Test.cpp' },
                region: { startLine: 17 }
            }
        }],
        ...overrides
    };
}

function writeArtifacts(root, values = {}) {
    const artifactRoot = path.join(root, `artifacts-${Math.random().toString(16).slice(2)}`);
    fs.mkdirSync(artifactRoot, { recursive: true });
    for (const language of EXPECTED_LANGUAGES) {
        if (values[language] === null) continue;
        const value = Object.hasOwn(values, language) ? values[language] : sarif(language);
        const content = typeof value === 'string' ? value : JSON.stringify(value);
        fs.writeFileSync(path.join(artifactRoot, ARTIFACT_FILES[language]), content, 'utf8');
    }
    return artifactRoot;
}

async function runPreflight(root, data) {
    const runtime = harness(data);
    const eventPath = writeEvent(root, data.event);
    await withEnvironment({
        REPORT_MODE: 'trusted-preflight',
        EXPECTED_LANGUAGES: JSON.stringify(EXPECTED_LANGUAGES),
        REPORTER_TEST_OUTCOME: 'success',
        WORKFLOW_RUN_EVENT_PATH: eventPath
    }, () => reportCodeqlFindings(runtime));
    return runtime;
}

async function runTrustedReport(root, data, manifest, artifactRoot, envOverrides = {}) {
    const runtime = harness(data);
    const eventPath = writeEvent(root, data.event);
    const summaryPath = path.join(root, `summary-${Math.random().toString(16).slice(2)}.json`);
    await withEnvironment({
        REPORT_MODE: 'trusted-report',
        ARTIFACT_DIR: artifactRoot,
        EXPECTED_LANGUAGES: JSON.stringify(EXPECTED_LANGUAGES),
        REPORTER_TEST_OUTCOME: 'success',
        PREFLIGHT_OUTCOME: 'success',
        PREFLIGHT_ARTIFACT_MANIFEST: manifest,
        DOWNLOAD_OUTCOME: 'success',
        SUMMARY_PATH: summaryPath,
        WORKFLOW_RUN_EVENT_PATH: eventPath,
        ...envOverrides
    }, () => reportCodeqlFindings(runtime));
    assert(fs.existsSync(summaryPath), 'trusted reporting must always emit a machine-readable summary');
    return { runtime, summary: JSON.parse(fs.readFileSync(summaryPath, 'utf8')) };
}

async function preflightAndReport(root, preflightData, reportData, artifactRoot, envOverrides = {}) {
    const preflight = await runPreflight(root, preflightData);
    assert.strictEqual(preflight.observed.outputs.authorized, 'true');
    assert.strictEqual(preflight.observed.failed.length, 0);
    assert.strictEqual(preflight.observed.outputs['artifact-ids'], '5000,5001,5002');
    return runTrustedReport(
        root,
        reportData,
        preflight.observed.outputs['artifact-manifest'],
        artifactRoot,
        envOverrides
    );
}

function assertNoMutation(result) {
    assert.strictEqual(result.runtime.observed.createdComments.length, 0);
    assert.strictEqual(result.runtime.observed.updatedComments.length, 0);
}

function testWorkflowShape() {
    const scanner = fs.readFileSync(path.join(__dirname, '..', 'workflows', 'codeql.yml'), 'utf8');
    const reporter = fs.readFileSync(path.join(__dirname, '..', 'workflows', 'codeql-report.yml'), 'utf8');
    const combined = `${scanner}\n${reporter}`;
    const actionUses = [...combined.matchAll(/^\s*uses:\s*([^\s#]+)/gm)].map(match => match[1]);
    const codeqlActionPin = 'a35ac6e6798d72df5475948b28efb89edc2e19ca';
    const codeqlUses = actionUses.filter(action => action.startsWith('github/codeql-action/'));
    const cCppBuildMode = scanner.match(
        /^[ \t]*-[ \t]*language:[ \t]*c-cpp[ \t]*\r?\n[ \t]*build-mode:[ \t]*([^\s#]+)/mi,
    );

    assert(!/^\s*(?:-\s*)?run:\s*/m.test(scanner), 'the source scanner must not execute repository code');
    assert(!scanner.includes('pull-requests: write'));
    assert(!scanner.includes('issues: write'));
    assert(cCppBuildMode, 'the C/C++ scanner matrix row must declare a build mode');
    assert.strictEqual(
        cCppBuildMode[1].toLowerCase(),
        'none',
        'the C/C++ scanner must use build-mode none; manual/autobuild are forbidden',
    );
    assert(scanner.includes('CODEQL_EXTRACTOR_CPP_OPTION_FRONTEND_OPTIONS: --c++23'),
        'buildless C/C++ extraction must override the pinned CLI default to SparkEngine C++23');
    assert(codeqlUses.length >= 2 && codeqlUses.every(action => action.endsWith(`@${codeqlActionPin}`)),
        'the C++23 extractor override must remain bound to the reviewed CodeQL Action 2.26.4 pin');
    assert(scanner.includes('config: |'));
    assert(!scanner.includes('config-file:'));
    assert(scanner.includes('archive: false'));
    assert(scanner.includes('repository: ${{ github.event.pull_request.head.repo.full_name || github.repository }}'));
    assert(scanner.includes('ref: ${{ github.event.pull_request.head.sha || github.sha }}'));
    assert(actionUses.length >= 7 && actionUses.every(action => /^[^@\s]+@[0-9a-f]{40}$/.test(action)),
        'all actions must be remote actions pinned to full commit SHAs');

    assert(reporter.includes('pull_request_target:'));
    assert(reporter.includes('types: [opened, synchronize, reopened]'));
    assert(reporter.includes("if: github.event_name == 'workflow_run'"));
    assert(reporter.includes("if: github.event_name == 'pull_request_target'"));
    const invalidation = reporter.slice(reporter.indexOf('  invalidate:'), reporter.indexOf('  report:'));
    assert(!invalidation.includes('actions/checkout@'), 'PR-head invalidation must not check out untrusted code');
    assert(invalidation.includes('spark-codeql-report-pending'));
    assert(invalidation.includes('pull.head.sha.toLowerCase()'));
    assert(reporter.includes('ref: ${{ github.event.repository.default_branch }}'));
    assert(reporter.includes('artifact-ids: ${{ steps.preflight.outputs.artifact-ids }}'));
    assert(reporter.includes('skip-decompress: true'));
    assert(reporter.includes('digest-mismatch: error'));
    assert(reporter.includes("require('./trusted-reporter/.github/scripts/report-codeql-findings.js')"));
    assert(reporter.includes('node trusted-reporter/.github/scripts/test-report-codeql-findings.js'));
}

function invalidationScript() {
    const reporter = fs.readFileSync(path.join(__dirname, '..', 'workflows', 'codeql-report.yml'), 'utf8');
    const step = reporter.indexOf('    - name: Mark prior CodeQL report pending');
    const marker = step >= 0 ? reporter.slice(step).match(/^        script: \|\r?$/m) : null;
    const start = marker ? step + marker.index + marker[0].length : -1;
    assert(step >= 0 && marker && start >= 0, 'invalidation script must be present in the trusted workflow');
    const lines = reporter.slice(start).replace(/^\r?\n/, '').split(/\r?\n/);
    const scriptLines = [];
    for (const line of lines) {
        if (line && !line.startsWith('          ')) break;
        scriptLines.push(line ? line.slice(10) : '');
    }
    return scriptLines.join('\n');
}

async function runInvalidation(comments, head = SOURCE_HEAD_SHA, liveHead = head) {
    const observed = { created: [], updated: [], info: [], warnings: [] };
    const repository = { id: REPOSITORY_ID, full_name: 'Krilliac/SparkEngine' };
    const headRepository = { id: HEAD_REPOSITORY_ID, full_name: 'contributor/SparkEngine' };
    const livePull = {
        number: 42, state: 'open', base: { repo: repository },
        head: { sha: liveHead, repo: headRepository }
    };
    const github = { rest: {
        pulls: { async get() { return { data: clone(livePull) }; } },
        issues: {
        async listComments() { return { data: clone(comments), headers: {} }; },
        async createComment(request) { observed.created.push(request); return { data: { id: 1 } }; },
        async updateComment(request) { observed.updated.push(request); return { data: { id: request.comment_id } }; }
    } } };
    const context = {
        repo: { owner: 'Krilliac', repo: 'SparkEngine' },
        payload: {
            repository,
            pull_request: {
                number: 42,
                state: 'open',
                base: { repo: repository },
                head: { sha: head, repo: headRepository }
            }
        }
    };
    const core = {
        info: message => observed.info.push(message),
        warning: message => observed.warnings.push(message)
    };
    const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
    await new AsyncFunction('github', 'context', 'core', invalidationScript())(github, context, core);
    return observed;
}

async function testInvalidation() {
    const ownedOld = [{
        id: 8001,
        body: `${MARKER}\n<!-- spark-codeql-report-state run-number=49 run-attempt=1 run-id=2999 pr-head=${CHANGED_SHA} run-head=${CHANGED_SHA} -->\nclean`,
        user: { type: 'Bot', login: 'github-actions[bot]' },
        performed_via_github_app: { slug: 'github-actions' }
    }];
    const invalidated = await runInvalidation(ownedOld);
    assert.strictEqual(invalidated.created.length, 0);
    assert.strictEqual(invalidated.updated.length, 1);
    assert.strictEqual(invalidated.updated[0].comment_id, 8001);
    assert(invalidated.updated[0].body.includes(`spark-codeql-report-pending pr-head=${SOURCE_HEAD_SHA}`));
    assert(!invalidationScript().includes('actions/checkout@'));

    const attacker = [{
        id: 8002,
        body: `${MARKER}\nattacker-controlled marker`,
        user: { type: 'User', login: 'attacker' }
    }];
    const spoof = await runInvalidation(attacker);
    assert.strictEqual(spoof.updated.length, 0);
    assert.strictEqual(spoof.created.length, 1);

    const current = clone(ownedOld);
    current[0].body = current[0].body.replaceAll(CHANGED_SHA, SOURCE_HEAD_SHA);
    const alreadyCurrent = await runInvalidation(current);
    assert.strictEqual(alreadyCurrent.created.length, 0);
    assert.strictEqual(alreadyCurrent.updated.length, 0,
        'late invalidation must not overwrite a completed report for the same head');

    const staleEvent = await runInvalidation(ownedOld, CHANGED_SHA, SOURCE_HEAD_SHA);
    assert.strictEqual(staleEvent.created.length, 0);
    assert.strictEqual(staleEvent.updated.length, 0,
        'an out-of-order invalidation event must not overwrite state for the current PR head');
}

async function main() {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'spark-codeql-report-'));
    try {
        testWorkflowShape();
        await testInvalidation();

        await assert.rejects(
            () => withEnvironment({ REPORT_MODE: 'collect' }, () => reportCodeqlFindings(harness(fixture()))),
            /not permitted/
        );

        const workflowMismatch = fixture();
        workflowMismatch.sourceWorkflowBlobSha = CHANGED_SHA;
        const rejectedWorkflow = await runPreflight(root, workflowMismatch);
        assert.strictEqual(rejectedWorkflow.observed.outputs.authorized, 'false');
        assert(rejectedWorkflow.observed.failed.some(message => message.includes('does not match')));
        assert.strictEqual(rejectedWorkflow.observed.listCommentsCalls, 0);
        assert.strictEqual(rejectedWorkflow.observed.artifactListCalls, 0,
            'identity failure must stop before enumerating attacker-controlled artifacts');
        assert.strictEqual(rejectedWorkflow.observed.pullListCalls.length, 0,
            'identity failure must stop before PR candidate enumeration');

        const badProvenance = fixture();
        badProvenance.artifacts[1].workflow_run.head_repository_id = 9999;
        const rejectedProvenance = await runPreflight(root, badProvenance);
        assert.strictEqual(rejectedProvenance.observed.outputs.authorized, 'false');
        assert(rejectedProvenance.observed.failed.some(message => message.includes('exact source run')));

        const badDigest = fixture();
        badDigest.artifacts[0].digest = 'sha256:not-a-digest';
        const rejectedDigest = await runPreflight(root, badDigest);
        assert.strictEqual(rejectedDigest.observed.outputs.authorized, 'false');
        assert(rejectedDigest.observed.failed.some(message => message.includes('valid SHA-256 digest')));

        const wrongRepository = fixture();
        wrongRepository.event.workflow_run.repository.id += 1;
        const rejectedRepository = await runPreflight(root, wrongRepository);
        assert.strictEqual(rejectedRepository.observed.outputs.authorized, 'false');
        assert(rejectedRepository.observed.failed.some(message => message.includes('expected repository')));

        const invalidRepositoryId = fixture();
        invalidRepositoryId.event.repository.id = 0;
        invalidRepositoryId.event.workflow_run.repository.id = 0;
        invalidRepositoryId.run.repository.id = 0;
        const rejectedRepositoryId = await runPreflight(root, invalidRepositoryId);
        assert.strictEqual(rejectedRepositoryId.observed.outputs.authorized, 'false');
        assert(rejectedRepositoryId.observed.failed.some(message => message.includes('expected repository')));

        const newerAttempt = fixture();
        newerAttempt.run.run_attempt = 2;
        const rejectedAttempt = await runPreflight(root, newerAttempt);
        assert.strictEqual(rejectedAttempt.observed.outputs.authorized, 'false');
        assert(rejectedAttempt.observed.failed.some(message => message.includes('newer attempt')));

        const missingApiArtifact = fixture();
        missingApiArtifact.artifacts.pop();
        const rejectedInventory = await runPreflight(root, missingApiArtifact);
        assert.strictEqual(rejectedInventory.observed.outputs.authorized, 'false');
        assert(rejectedInventory.observed.failed.some(message => message.includes("exactly one 'python.sarif'")));

        const excessiveInventory = fixture();
        excessiveInventory.artifacts = Array.from({ length: 101 }, (_, index) => ({
            ...clone(excessiveInventory.artifacts[index % excessiveInventory.artifacts.length]),
            id: 6000 + index,
            name: `attacker-${index}.sarif`
        }));
        const rejectedExcessiveInventory = await runPreflight(root, excessiveInventory);
        assert.strictEqual(rejectedExcessiveInventory.observed.outputs.authorized, 'false');
        assert(rejectedExcessiveInventory.observed.failed.some(message => message.includes('100-item limit')));

        const directArtifactRootArray = fixture();
        directArtifactRootArray.directArtifactRootArray = true;
        const rejectedDirectArtifactRootArray = await runPreflight(root, directArtifactRootArray);
        assert.strictEqual(rejectedDirectArtifactRootArray.observed.outputs.authorized, 'false');
        assert.strictEqual(rejectedDirectArtifactRootArray.observed.outputs['artifact-ids'], '');
        assert.strictEqual(rejectedDirectArtifactRootArray.observed.outputs['artifact-manifest'], '[]');
        assert(rejectedDirectArtifactRootArray.observed.failed.some(message =>
            message.includes("API response field 'artifacts' is not an array")));

        const malformedNormalizedArtifactPage = fixture();
        malformedNormalizedArtifactPage.useNormalizedPaginateIterator = true;
        malformedNormalizedArtifactPage.normalizedPaginatePageOverrides = { artifacts: { unexpected: [] } };
        const rejectedMalformedNormalizedArtifactPage = await runPreflight(root, malformedNormalizedArtifactPage);
        assert.strictEqual(rejectedMalformedNormalizedArtifactPage.observed.outputs.authorized, 'false');
        assert.strictEqual(rejectedMalformedNormalizedArtifactPage.observed.outputs['artifact-ids'], '');
        assert(rejectedMalformedNormalizedArtifactPage.observed.failed.some(message =>
            message.includes("API response field 'artifacts' is not an array")));

        const normalizedPaginationData = fixture();
        normalizedPaginationData.useNormalizedPaginateIterator = true;
        normalizedPaginationData.includeNormalizedPaginationMetadata = true;
        const normalizedPagination = await preflightAndReport(
            root, normalizedPaginationData, normalizedPaginationData, writeArtifacts(root));
        assert.strictEqual(normalizedPagination.summary.status, 'complete',
            'Octokit-normalized root arrays must be accepted for wrapped paginated lists');
        assert.deepStrictEqual(normalizedPagination.runtime.observed.normalizedPaginateFields,
            ['artifacts', 'workflow_runs'],
            'the live iterator shape must cover artifact and same-commit run pagination');

        const missingPullReferences = fixture();
        missingPullReferences.event.workflow_run.pull_requests = [];
        missingPullReferences.run.pull_requests = [];
        const resolvedAssociation = await runPreflight(root, missingPullReferences);
        assert.strictEqual(resolvedAssociation.observed.outputs.authorized, 'true');
        assert.strictEqual(resolvedAssociation.observed.pullListCalls.length, 1);
        assert.strictEqual(resolvedAssociation.observed.pullListCalls[0].head, 'contributor:hostile-sarif');
        assert(resolvedAssociation.observed.getContentCalls.some(request =>
            request.owner === 'contributor' && request.repo === 'SparkEngine' && request.ref === SOURCE_HEAD_SHA),
        'source workflow blob must be fetched from the fork head repository');

        const mismatchedSelector = fixture();
        mismatchedSelector.event.workflow_run.pull_requests = [];
        mismatchedSelector.run.pull_requests = [];
        mismatchedSelector.pullCandidates = [clone(mismatchedSelector.pull)];
        mismatchedSelector.pullCandidates[0].head.repo.id = 9999;
        const rejectedSelector = await runPreflight(root, mismatchedSelector);
        assert.strictEqual(rejectedSelector.observed.outputs.authorized, 'false');
        assert(rejectedSelector.observed.failed.some(message => /exact source pull request/i.test(message)));

        const ambiguousSelector = fixture();
        ambiguousSelector.event.workflow_run.pull_requests = [];
        ambiguousSelector.run.pull_requests = [];
        const secondPull = clone(ambiguousSelector.pullCandidates[0]);
        secondPull.number = 43;
        ambiguousSelector.pullCandidates.push(secondPull);
        const rejectedAmbiguity = await runPreflight(root, ambiguousSelector);
        assert.strictEqual(rejectedAmbiguity.observed.outputs.authorized, 'false');
        assert(rejectedAmbiguity.observed.failed.some(message => message.includes('found 2')));

        const cleanData = fixture();
        const clean = await preflightAndReport(root, cleanData, cleanData, writeArtifacts(root));
        assert.strictEqual(clean.summary.status, 'complete');
        assert.strictEqual(clean.summary.uniqueFindings, 0);
        assert.deepStrictEqual(clean.summary.completedLanguages, EXPECTED_LANGUAGES);
        assert.strictEqual(clean.summary.artifactEvidence.length, 3);
        assert.strictEqual(clean.runtime.observed.createdComments.length, 1);
        assert(clean.runtime.observed.createdComments[0].body.includes('valid SARIF and no findings'));

        const warningNotificationData = fixture();
        const warningNotification = await preflightAndReport(
            root,
            warningNotificationData,
            warningNotificationData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: [{
                            level: 'warning',
                            descriptor: { id: 'cpp/diagnostics/example-warning' },
                            message: { text: 'Non-fatal diagnostic.' }
                        }]
                    }]
                })
            })
        );
        assert.strictEqual(warningNotification.summary.status, 'complete');

        const omittedWarningDescriptorData = fixture();
        const omittedWarningDescriptor = await preflightAndReport(
            root,
            omittedWarningDescriptorData,
            omittedWarningDescriptorData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: [{
                            level: 'warning',
                            message: { text: 'Extraction failed in SparkEngine/Source/Broken.cpp.' }
                        }]
                    }]
                })
            })
        );
        assert.strictEqual(omittedWarningDescriptor.summary.status, 'incomplete');
        assert(omittedWarningDescriptor.summary.evidenceErrors.some(error =>
            error.includes('has no resolvable descriptor identity')));

        const emptyWarningDescriptorData = fixture();
        const emptyWarningDescriptor = await preflightAndReport(
            root,
            emptyWarningDescriptorData,
            emptyWarningDescriptorData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: [{
                            level: 'warning',
                            descriptor: {},
                            message: { text: 'Extraction failed in SparkEngine/Source/Broken.cpp.' }
                        }]
                    }]
                })
            })
        );
        assert.strictEqual(emptyWarningDescriptor.summary.status, 'incomplete');
        assert(emptyWarningDescriptor.summary.evidenceErrors.some(error =>
            error.includes('has no resolvable descriptor identity')));

        const extractionWarningData = fixture();
        const extractionWarning = await preflightAndReport(
            root,
            extractionWarningData,
            extractionWarningData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: [{
                            level: 'warning',
                            descriptor: { id: 'cpp/diagnostics/extraction-warnings', index: 1 },
                            message: {
                                text: 'Extraction failed in SparkEngine/Source/Broken.cpp with warning compiler exited early.'
                            }
                        }]
                    }]
                })
            })
        );
        assert.strictEqual(extractionWarning.summary.status, 'incomplete');
        assert.deepStrictEqual(extractionWarning.summary.invalidLanguages, ['c-cpp']);
        assert(extractionWarning.summary.evidenceErrors.some(error =>
            error.includes("extraction-failure diagnostic 'cpp/diagnostics/extraction-warnings'")));
        assert(extractionWarning.runtime.observed.failed.length >= 1);

        const slashExtractionWarningData = fixture();
        const slashExtractionWarning = await preflightAndReport(
            root,
            slashExtractionWarningData,
            slashExtractionWarningData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: [{
                            level: 'warning',
                            descriptor: { id: 'cpp/diagnostics/extraction-warnings/', index: 1 },
                            message: {
                                text: 'Extraction failed in SparkEngine/Source/Broken.cpp with warning compiler exited early.'
                            }
                        }]
                    }]
                })
            })
        );
        assert.strictEqual(slashExtractionWarning.summary.status, 'incomplete');
        assert(slashExtractionWarning.summary.evidenceErrors.some(error =>
            error.includes("extraction-failure diagnostic 'cpp/diagnostics/extraction-warnings/'")),
        'an empty child component must retain the extraction-failure identity');

        const missingDiagnosticsData = fixture();
        const missingDiagnosticsSarif = sarif('c-cpp');
        missingDiagnosticsSarif.runs[0].tool.driver.notifications = [{
            id: 'cpp/diagnostics/successfully-extracted-files'
        }];
        const missingDiagnostics = await preflightAndReport(
            root,
            missingDiagnosticsData,
            missingDiagnosticsData,
            writeArtifacts(root, { 'c-cpp': missingDiagnosticsSarif })
        );
        assert.strictEqual(missingDiagnostics.summary.status, 'incomplete');
        assert.deepStrictEqual(missingDiagnostics.summary.invalidLanguages, ['c-cpp']);
        assert(missingDiagnostics.summary.evidenceErrors.some(error =>
            error.includes('missing mandatory CodeQL diagnostic descriptors') &&
            error.includes('cpp/diagnostics/extraction-warnings') &&
            error.includes('cpp/diagnostics/failed-extractor-invocations')));

        const extensionDiagnosticsData = fixture();
        const extensionDiagnosticsSarif = sarif('c-cpp');
        const cppDiagnostics = extensionDiagnosticsSarif.runs[0].tool.driver.notifications;
        extensionDiagnosticsSarif.runs[0].tool.driver.notifications = cppDiagnostics.slice(0, 1);
        extensionDiagnosticsSarif.runs[0].tool.extensions = [{
            name: 'codeql/cpp-queries',
            notifications: cppDiagnostics.slice(1)
        }];
        const extensionDiagnostics = await preflightAndReport(
            root,
            extensionDiagnosticsData,
            extensionDiagnosticsData,
            writeArtifacts(root, { 'c-cpp': extensionDiagnosticsSarif })
        );
        assert.strictEqual(extensionDiagnostics.summary.status, 'complete',
            'mandatory diagnostic descriptors may be split across driver and extension components');

        const indexedExtensionFailureData = fixture();
        const indexedExtensionFailureSarif = clone(extensionDiagnosticsSarif);
        indexedExtensionFailureSarif.runs[0].invocations = [{
            executionSuccessful: true,
            toolExecutionNotifications: [{
                level: 'warning',
                descriptor: { index: 0, toolComponent: { index: 0 } },
                message: {
                    text: 'Extraction failed in SparkEngine/Source/Broken.cpp with warning compiler exited early.'
                }
            }]
        }];
        const indexedExtensionFailure = await preflightAndReport(
            root,
            indexedExtensionFailureData,
            indexedExtensionFailureData,
            writeArtifacts(root, { 'c-cpp': indexedExtensionFailureSarif })
        );
        assert.strictEqual(indexedExtensionFailure.summary.status, 'incomplete');
        assert(indexedExtensionFailure.summary.evidenceErrors.some(error =>
            error.includes("extraction-failure diagnostic 'cpp/diagnostics/extraction-warnings'")),
        'index-only SARIF references into extension notification descriptors must resolve and fail closed');

        const childIdConfusionData = fixture();
        const childIdConfusionSarif = sarif('c-cpp');
        childIdConfusionSarif.runs[0].tool.driver.notifications.push({ id: 'cpp/diagnostics' });
        childIdConfusionSarif.runs[0].invocations = [{
            executionSuccessful: true,
            toolExecutionNotifications: [{
                level: 'warning',
                descriptor: { id: 'cpp/diagnostics/extraction-warnings', index: 3 },
                message: {
                    text: 'Extraction failed in SparkEngine/Source/Broken.cpp with warning compiler exited early.'
                }
            }]
        }];
        const childIdConfusion = await preflightAndReport(
            root,
            childIdConfusionData,
            childIdConfusionData,
            writeArtifacts(root, { 'c-cpp': childIdConfusionSarif })
        );
        assert.strictEqual(childIdConfusion.summary.status, 'incomplete');
        assert(childIdConfusion.summary.evidenceErrors.some(error =>
            error.includes("extraction-failure diagnostic 'cpp/diagnostics/extraction-warnings'")),
        'a specific extraction-failure ID must not be downgraded to its broader indexed descriptor');

        const duplicateComponentGuidData = fixture();
        const duplicateComponentGuidSarif = sarif('c-cpp');
        duplicateComponentGuidSarif.runs[0].tool.driver.guid =
            'AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA';
        duplicateComponentGuidSarif.runs[0].tool.extensions = [{
            name: 'duplicate-component',
            guid: 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
            notifications: []
        }];
        const duplicateComponentGuid = await preflightAndReport(
            root,
            duplicateComponentGuidData,
            duplicateComponentGuidData,
            writeArtifacts(root, { 'c-cpp': duplicateComponentGuidSarif })
        );
        assert.strictEqual(duplicateComponentGuid.summary.status, 'incomplete');
        assert(duplicateComponentGuid.summary.evidenceErrors.some(error =>
            error.includes('duplicate tool component GUID')));

        const duplicateDescriptorGuidData = fixture();
        const duplicateDescriptorGuidSarif = sarif('c-cpp');
        duplicateDescriptorGuidSarif.runs[0].tool.driver.notifications[0].guid =
            'BBBBBBBB-BBBB-4BBB-8BBB-BBBBBBBBBBBB';
        duplicateDescriptorGuidSarif.runs[0].tool.driver.notifications[1].guid =
            'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb';
        const duplicateDescriptorGuid = await preflightAndReport(
            root,
            duplicateDescriptorGuidData,
            duplicateDescriptorGuidData,
            writeArtifacts(root, { 'c-cpp': duplicateDescriptorGuidSarif })
        );
        assert.strictEqual(duplicateDescriptorGuid.summary.status, 'incomplete');
        assert(duplicateDescriptorGuid.summary.evidenceErrors.some(error =>
            error.includes('duplicate notification descriptor GUID')));

        const duplicateDescriptorIdData = fixture();
        const duplicateDescriptorIdSarif = sarif('c-cpp');
        duplicateDescriptorIdSarif.runs[0].tool.extensions = [{
            name: 'duplicate-descriptor',
            notifications: [{ id: 'cpp/diagnostics/extraction-warnings' }]
        }];
        const duplicateDescriptorId = await preflightAndReport(
            root,
            duplicateDescriptorIdData,
            duplicateDescriptorIdData,
            writeArtifacts(root, { 'c-cpp': duplicateDescriptorIdSarif })
        );
        assert.strictEqual(duplicateDescriptorId.summary.status, 'incomplete');
        assert(duplicateDescriptorId.summary.evidenceErrors.some(error =>
            error.includes('duplicate notification descriptor ID')));

        const malformedComponentGuidData = fixture();
        const malformedComponentGuidSarif = sarif('c-cpp');
        malformedComponentGuidSarif.runs[0].tool.driver.guid =
            'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaZ';
        const malformedComponentGuid = await preflightAndReport(
            root,
            malformedComponentGuidData,
            malformedComponentGuidData,
            writeArtifacts(root, { 'c-cpp': malformedComponentGuidSarif })
        );
        assert.strictEqual(malformedComponentGuid.summary.status, 'incomplete');
        assert(malformedComponentGuid.summary.evidenceErrors.some(error =>
            error.includes('tool.driver has an invalid GUID')));

        const malformedDescriptorGuidData = fixture();
        const malformedDescriptorGuidSarif = sarif('c-cpp');
        malformedDescriptorGuidSarif.runs[0].tool.driver.notifications[0].guid =
            'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbZ';
        const malformedDescriptorGuid = await preflightAndReport(
            root,
            malformedDescriptorGuidData,
            malformedDescriptorGuidData,
            writeArtifacts(root, { 'c-cpp': malformedDescriptorGuidSarif })
        );
        assert.strictEqual(malformedDescriptorGuid.summary.status, 'incomplete');
        assert(malformedDescriptorGuid.summary.evidenceErrors.some(error =>
            error.includes('tool.driver.notifications[0] has an invalid GUID')));

        const malformedComponentReferenceGuidData = fixture();
        const malformedComponentReferenceGuidSarif = sarif('c-cpp');
        malformedComponentReferenceGuidSarif.runs[0].invocations = [{
            executionSuccessful: true,
            toolExecutionNotifications: [{
                level: 'warning',
                descriptor: {
                    id: 'cpp/diagnostics/example-warning',
                    toolComponent: { guid: 'cccccccc-cccc-4ccc-8ccc-cccccccccccZ' }
                }
            }]
        }];
        const malformedComponentReferenceGuid = await preflightAndReport(
            root,
            malformedComponentReferenceGuidData,
            malformedComponentReferenceGuidData,
            writeArtifacts(root, { 'c-cpp': malformedComponentReferenceGuidSarif })
        );
        assert.strictEqual(malformedComponentReferenceGuid.summary.status, 'incomplete');
        assert(malformedComponentReferenceGuid.summary.evidenceErrors.some(error =>
            error.includes('invalid tool component GUID')));

        const malformedDescriptorReferenceGuidData = fixture();
        const malformedDescriptorReferenceGuidSarif = sarif('c-cpp');
        malformedDescriptorReferenceGuidSarif.runs[0].invocations = [{
            executionSuccessful: true,
            toolExecutionNotifications: [{
                level: 'warning',
                descriptor: { guid: 'dddddddd-dddd-4ddd-8ddd-dddddddddddZ' }
            }]
        }];
        const malformedDescriptorReferenceGuid = await preflightAndReport(
            root,
            malformedDescriptorReferenceGuidData,
            malformedDescriptorReferenceGuidData,
            writeArtifacts(root, { 'c-cpp': malformedDescriptorReferenceGuidSarif })
        );
        assert.strictEqual(malformedDescriptorReferenceGuid.summary.status, 'incomplete');
        assert(malformedDescriptorReferenceGuid.summary.evidenceErrors.some(error =>
            error.includes('invalid descriptor GUID')));

        const failedInvocationData = fixture();
        const failedInvocation = await preflightAndReport(
            root,
            failedInvocationData,
            failedInvocationData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{ executionSuccessful: false }]
                })
            })
        );
        assert.strictEqual(failedInvocation.summary.status, 'incomplete');
        assert.deepStrictEqual(failedInvocation.summary.invalidLanguages, ['c-cpp']);
        assert(failedInvocation.summary.evidenceErrors.some(error => error.includes('did not complete successfully')));
        assert(failedInvocation.runtime.observed.failed.length >= 1);

        const extractorErrorData = fixture();
        const extractorError = await preflightAndReport(
            root,
            extractorErrorData,
            extractorErrorData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: [{
                            level: 'error',
                            descriptor: { id: 'cpp/diagnostics/failed-extractor-invocations' },
                            message: { text: 'Extraction aborted for compiler invocation.' }
                        }]
                    }]
                })
            })
        );
        assert.strictEqual(extractorError.summary.status, 'incomplete');
        assert.deepStrictEqual(extractorError.summary.invalidLanguages, ['c-cpp']);
        assert(extractorError.summary.evidenceErrors.some(error =>
            error.includes('cpp/diagnostics/failed-extractor-invocations')));

        const missingInvocationData = fixture();
        const missingInvocation = await preflightAndReport(
            root,
            missingInvocationData,
            missingInvocationData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', { invocations: [] })
            })
        );
        assert.strictEqual(missingInvocation.summary.status, 'incomplete');
        assert(missingInvocation.summary.evidenceErrors.some(error => error.includes('no invocation evidence')));

        const excessiveNotificationsData = fixture();
        const excessiveNotifications = Array.from({ length: 10001 }, () => ({ level: 'none' }));
        const rejectedNotifications = await preflightAndReport(
            root,
            excessiveNotificationsData,
            excessiveNotificationsData,
            writeArtifacts(root, {
                'c-cpp': sarif('c-cpp', [], 'c-cpp', {
                    invocations: [{
                        executionSuccessful: true,
                        toolExecutionNotifications: excessiveNotifications
                    }]
                })
            })
        );
        assert.strictEqual(rejectedNotifications.summary.status, 'incomplete');
        assert(rejectedNotifications.summary.evidenceErrors.some(error => error.includes('notification limit')));

        const excessiveDescriptorsData = fixture();
        const excessiveDescriptorsSarif = sarif('c-cpp');
        excessiveDescriptorsSarif.runs[0].tool.driver.notifications.push(
            ...Array.from({ length: 10000 }, (_, index) => ({ id: `codeql/test-diagnostic-${index}` }))
        );
        const rejectedDescriptors = await preflightAndReport(
            root,
            excessiveDescriptorsData,
            excessiveDescriptorsData,
            writeArtifacts(root, { 'c-cpp': excessiveDescriptorsSarif })
        );
        assert.strictEqual(rejectedDescriptors.summary.status, 'incomplete');
        assert(rejectedDescriptors.summary.evidenceErrors.some(error =>
            error.includes('notification descriptor limit')));

        const hostile = finding({
            ruleId: 'cpp/evil|@reviewers[rule]',
            message: {
                text: '@reviewers [click](https://evil.example) **bold** <details> | `tick`\nnext'
            },
            locations: [{
                physicalLocation: {
                    artifactLocation: { uri: 'bad|@team[link](https://evil.example).cpp' },
                    region: { startLine: 9 }
                }
            }]
        });
        const hostileData = fixture();
        const hostileResult = await preflightAndReport(root, hostileData, hostileData, writeArtifacts(root, {
            'c-cpp': sarif('c-cpp', [hostile])
        }));
        const hostileBody = hostileResult.runtime.observed.createdComments[0].body;
        assert.strictEqual(hostileResult.summary.status, 'complete');
        assert(hostileBody.includes('&#64;reviewers'));
        assert(hostileBody.includes('\\[click\\]\\('));
        assert(hostileBody.includes('&lt;details&gt;'));
        assert(!hostileBody.includes('@reviewers'));
        assert(!hostileBody.includes('[click](https://evil.example)'));

        const manyFindings = Array.from({ length: 350 }, (_, index) => finding({
            ruleId: `@reviewers/${'\u{1f4a3}'.repeat(120)}`,
            level: ['error', 'warning', 'note'][index % 3],
            message: { text: `@reviewers hostile-${index} ${'\u{1f4a3}'.repeat(300)}` },
            locations: [{
                physicalLocation: {
                    artifactLocation: { uri: `bad|@team/${'\u{1f4a3}'.repeat(300)}-${index}.cpp` },
                    region: { startLine: index + 1 }
                }
            }]
        }));
        const boundedData = fixture();
        const bounded = await preflightAndReport(root, boundedData, boundedData, writeArtifacts(root, {
            'c-cpp': sarif('c-cpp', manyFindings)
        }));
        const boundedBody = bounded.runtime.observed.createdComments[0].body;
        assert.strictEqual(bounded.summary.uniqueFindings, 350);
        assert.strictEqual(bounded.summary.retainedFindings, 300);
        assert.strictEqual(bounded.summary.omittedFindings, 50);
        assert.strictEqual(bounded.summary.findings.length, 300);
        assert(bounded.summary.errors.length <= 100 && bounded.summary.warnings.length <= 100);
        assert(Buffer.byteLength(boundedBody, 'utf8') <= 65000);
        assert(boundedBody.includes(`**Analyzed commit:** \`${SOURCE_HEAD_SHA}\``),
            'bounded report body must retain analyzed-commit provenance');
        assert(boundedBody.includes('*Updated:'), 'bounded report body must retain its provenance footer');
        assert(!boundedBody.includes('@reviewers'));

        const forcedTruncation = reportCodeqlFindings._test.finishComment(
            ['\u{1f4a3}'.repeat(40000)],
            ['', `**Analyzed commit:** \`${SOURCE_HEAD_SHA}\``, '', '*Updated: test*']
        );
        assert(Buffer.byteLength(forcedTruncation, 'utf8') <= 65000);
        assert(forcedTruncation.includes('Report body truncated'));
        assert(forcedTruncation.includes(`**Analyzed commit:** \`${SOURCE_HEAD_SHA}\``));
        assert(forcedTruncation.endsWith('*Updated: test*'));

        const excessiveResultsData = fixture();
        const excessiveResults = Array.from({ length: 10001 }, () => finding());
        const rejectedResults = await preflightAndReport(root, excessiveResultsData, excessiveResultsData,
            writeArtifacts(root, { 'c-cpp': sarif('c-cpp', excessiveResults) }));
        assert.strictEqual(rejectedResults.summary.status, 'incomplete');
        assert(rejectedResults.summary.evidenceErrors.some(error => error.includes('result limit')));

        const malformedData = fixture();
        const malformed = await preflightAndReport(root, malformedData, malformedData, writeArtifacts(root, {
            actions: '{not-json'
        }));
        assert.strictEqual(malformed.summary.status, 'incomplete');
        assert.deepStrictEqual(malformed.summary.invalidLanguages, ['actions']);
        assert(malformed.summary.evidenceErrors.some(error => error.includes('invalid JSON')));
        assert(malformed.runtime.observed.failed.length >= 1);
        assert(!malformed.runtime.observed.createdComments[0].body.includes(':white_check_mark:'));

        const missingData = fixture();
        const missing = await preflightAndReport(root, missingData, missingData, writeArtifacts(root, { python: null }));
        assert.strictEqual(missing.summary.status, 'incomplete');
        assert.deepStrictEqual(missing.summary.missingLanguages, ['python']);
        assert(missing.summary.evidenceErrors.some(error => error.includes("'python.sarif' is missing")));
        assert(missing.runtime.observed.failed.length >= 1);

        const languageData = fixture();
        const wrongLanguage = await preflightAndReport(root, languageData, languageData, writeArtifacts(root, {
            'c-cpp': sarif('c-cpp', [], 'python')
        }));
        assert.strictEqual(wrongLanguage.summary.status, 'incomplete');
        assert.deepStrictEqual(wrongLanguage.summary.invalidLanguages, ['c-cpp']);
        assert(wrongLanguage.summary.evidenceErrors.some(error => error.includes("expected 'c-cpp' language")));

        const manifestData = fixture();
        const manifestPreflight = await runPreflight(root, manifestData);
        const changedManifest = JSON.parse(manifestPreflight.observed.outputs['artifact-manifest']);
        changedManifest[0].digest = `sha256:${'f'.repeat(64)}`;
        const manifestMismatch = await runTrustedReport(
            root,
            manifestData,
            JSON.stringify(changedManifest),
            writeArtifacts(root)
        );
        assert.strictEqual(manifestMismatch.summary.status, 'incomplete');
        assert(manifestMismatch.summary.evidenceErrors.some(error => error.includes('manifest changed')));

        const currentHeadData = fixture();
        const changedHead = fixture();
        changedHead.pull.head.sha = CHANGED_SHA;
        const staleHead = await preflightAndReport(root, currentHeadData, changedHead, writeArtifacts(root));
        assert.strictEqual(staleHead.summary.status, 'incomplete');
        assert(staleHead.summary.evidenceErrors.some(error => /exact source pull request/i.test(error)));
        assertNoMutation(staleHead);

        const newerRunData = fixture();
        newerRunData.useNormalizedPaginateIterator = true;
        const newerRun = clone(newerRunData.run);
        newerRun.id += 1;
        newerRun.run_number += 1;
        newerRunData.workflowRuns = [newerRun, newerRunData.run];
        const staleRun = await preflightAndReport(root, newerRunData, newerRunData, writeArtifacts(root));
        assert.strictEqual(staleRun.summary.status, 'stale');
        assert(staleRun.summary.staleReasons.some(reason => reason.includes('newer source workflow run')));
        assert.deepStrictEqual(staleRun.runtime.observed.normalizedPaginateFields, ['artifacts', 'workflow_runs']);
        assertNoMutation(staleRun);

        const directWorkflowRunsRootArray = fixture();
        directWorkflowRunsRootArray.directWorkflowRunsRootArray = true;
        const directWorkflowRunsPreflight = await runPreflight(root, directWorkflowRunsRootArray);
        assert.strictEqual(directWorkflowRunsPreflight.observed.outputs.authorized, 'true');
        const rejectedDirectWorkflowRunsRootArray = await runTrustedReport(
            root,
            directWorkflowRunsRootArray,
            directWorkflowRunsPreflight.observed.outputs['artifact-manifest'],
            writeArtifacts(root)
        );
        assert.strictEqual(rejectedDirectWorkflowRunsRootArray.summary.status, 'incomplete');
        assert.strictEqual(rejectedDirectWorkflowRunsRootArray.summary.mutation.reason, 'api-error');
        assert(rejectedDirectWorkflowRunsRootArray.summary.evidenceErrors.some(error =>
            error.includes("API response field 'workflow_runs' is not an array")));
        assertNoMutation(rejectedDirectWorkflowRunsRootArray);

        const malformedNormalizedWorkflowRunsPage = fixture();
        malformedNormalizedWorkflowRunsPage.useNormalizedPaginateIterator = true;
        malformedNormalizedWorkflowRunsPage.normalizedPaginatePageOverrides = {
            workflow_runs: { unexpected: [] }
        };
        const malformedWorkflowRunsPreflight = await runPreflight(root, malformedNormalizedWorkflowRunsPage);
        assert.strictEqual(malformedWorkflowRunsPreflight.observed.outputs.authorized, 'true');
        const rejectedMalformedNormalizedWorkflowRunsPage = await runTrustedReport(
            root,
            malformedNormalizedWorkflowRunsPage,
            malformedWorkflowRunsPreflight.observed.outputs['artifact-manifest'],
            writeArtifacts(root)
        );
        assert.strictEqual(rejectedMalformedNormalizedWorkflowRunsPage.summary.status, 'incomplete');
        assert.strictEqual(rejectedMalformedNormalizedWorkflowRunsPage.summary.mutation.reason, 'api-error');
        assert(rejectedMalformedNormalizedWorkflowRunsPage.summary.evidenceErrors.some(error =>
            error.includes("API response field 'workflow_runs' is not an array")));
        assertNoMutation(rejectedMalformedNormalizedWorkflowRunsPage);

        const spoofData = fixture();
        spoofData.comments = [{
            id: 7001,
            body: `${MARKER}\n<!-- spark-codeql-report-state run-number=999 run-attempt=1 run-id=999 pr-head=${SOURCE_HEAD_SHA} run-head=${SOURCE_HEAD_SHA} -->`,
            user: { type: 'User', login: 'attacker' }
        }];
        const spoof = await preflightAndReport(root, fixture(), spoofData, writeArtifacts(root));
        assert.strictEqual(spoof.runtime.observed.updatedComments.length, 0);
        assert.strictEqual(spoof.runtime.observed.createdComments.length, 1);

        const ownedData = fixture();
        ownedData.comments = [{
            id: 7002,
            body: `${MARKER}\nold report`,
            user: { type: 'Bot', login: 'github-actions[bot]' },
            performed_via_github_app: { slug: 'github-actions' }
        }];
        const owned = await preflightAndReport(root, fixture(), ownedData, writeArtifacts(root));
        assert.strictEqual(owned.runtime.observed.createdComments.length, 0);
        assert.strictEqual(owned.runtime.observed.updatedComments.length, 1);
        assert.strictEqual(owned.runtime.observed.updatedComments[0].comment_id, 7002);

        const pagedCommentsData = fixture();
        pagedCommentsData.commentLastPage = 20;
        pagedCommentsData.commentPages = {
            1: [{ id: 1, body: 'ordinary first-page comment', user: { type: 'User', login: 'user' } }],
            20: [{
                id: 7020,
                body: `${MARKER}\nold paged report`,
                user: { type: 'Bot', login: 'github-actions[bot]' },
                performed_via_github_app: { slug: 'github-actions' }
            }]
        };
        const pagedComments = await preflightAndReport(root, fixture(), pagedCommentsData, writeArtifacts(root));
        assert.strictEqual(pagedComments.runtime.observed.listCommentsCalls, 5);
        assert.deepStrictEqual(pagedComments.runtime.observed.commentRequests.map(request => request.page),
            [1, 17, 18, 19, 20]);
        assert.strictEqual(pagedComments.runtime.observed.createdComments.length, 0);
        assert.strictEqual(pagedComments.runtime.observed.updatedComments[0].comment_id, 7020);
        assert(pagedComments.runtime.observed.warnings.some(message => message.includes('bounded')));

        const newerCommentData = fixture();
        newerCommentData.comments = [{
            id: 7003,
            body: `${MARKER}\n<!-- spark-codeql-report-state run-number=51 run-attempt=1 run-id=9999 pr-head=${SOURCE_HEAD_SHA} run-head=${SOURCE_HEAD_SHA} -->`,
            user: { type: 'Bot', login: 'github-actions[bot]' },
            performed_via_github_app: { slug: 'github-actions' }
        }];
        const newerComment = await preflightAndReport(root, fixture(), newerCommentData, writeArtifacts(root));
        assert.strictEqual(newerComment.summary.status, 'stale');
        assertNoMutation(newerComment);

        console.log('report-codeql-findings trusted scenarios passed');
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
