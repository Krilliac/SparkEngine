const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const reportCiErrors = require('./report-ci-errors.js');

// `artifacts` mirrors actions/download-artifact with merge-multiple: false —
// every ci-errors-* artifact lands in its own subdirectory.
async function runScenario(name, needs, artifacts) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'spark-ci-report-'));
    const artifactDir = path.join(root, 'artifacts');
    const observed = { failed: [], info: [], warnings: [], summary: '' };

    try {
        const list = Array.isArray(artifacts) ? artifacts : (artifacts ? [artifacts] : []);
        for (const artifact of list) {
            const dir = path.join(artifactDir, `ci-errors-${artifact.job}`);
            fs.mkdirSync(dir, { recursive: true });
            fs.writeFileSync(path.join(dir, 'error-summary.json'), JSON.stringify(artifact), 'utf8');
        }

        process.env.ARTIFACT_DIR = artifactDir;
        process.env.NEEDS_JSON = typeof needs === 'string' ? needs : JSON.stringify(needs);

        const summary = {
            addRaw(body) {
                observed.summary = body;
                return this;
            },
            async write() {}
        };
        const core = {
            info: message => observed.info.push(message),
            warning: message => observed.warnings.push(message),
            setFailed: message => observed.failed.push(message),
            summary
        };

        await reportCiErrors({
            github: { rest: { issues: {} } },
            context: { repo: { owner: 'Krilliac', repo: 'SparkEngine' }, issue: {} },
            core,
            glob: {},
            io: {}
        });
        return observed;
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
        delete process.env.ARTIFACT_DIR;
        delete process.env.NEEDS_JSON;
    }
}

const msanArtifact = {
    job: 'build-linux-msan',
    errors: ['SUMMARY: MemorySanitizer: use-of-uninitialized-value in memcmp'],
    warnings: [],
    tests: []
};

async function main() {
    const success = await runScenario('success', {
        'build-linux-gcc': { result: 'success' },
        'build-linux-mingw-wine': { result: 'skipped' }
    });
    assert.deepStrictEqual(success.failed, []);
    assert(success.info.includes('No errors to report'));

    for (const result of ['failure', 'cancelled']) {
        const missingArtifact = await runScenario(`missing-${result}`, {
            'validate-prompts': { result }
        });
        assert.strictEqual(missingArtifact.failed.length, 1);
        assert(missingArtifact.summary.includes('validate-prompts'));
        assert(missingArtifact.summary.includes(`concluded '${result}'`));
    }

    const malformed = await runScenario('malformed', '{not-json');
    assert.strictEqual(malformed.failed.length, 1);
    assert.strictEqual(malformed.warnings.length, 1);
    assert(malformed.summary.includes('Could not parse required job results'));

    const artifactFailure = await runScenario(
        'artifact-failure',
        { coverage: { result: 'failure' } },
        { job: 'coverage', errors: ['coverage.cpp:12:3: error: threshold failed'], warnings: [], tests: [] }
    );
    assert.strictEqual(artifactFailure.failed.length, 1);
    assert(artifactFailure.summary.includes('threshold failed'));
    assert(artifactFailure.summary.includes(':x: CI Error Report'));

    // A job-level continue-on-error lane reports success through `needs` even
    // when it published an error summary: visible, but never a failure.
    const advisory = await runScenario('advisory', { 'build-linux-msan': { result: 'success' } }, msanArtifact);
    assert.deepStrictEqual(advisory.failed, []);
    assert.strictEqual(advisory.warnings.length, 1);
    assert(advisory.warnings[0].includes('advisory lanes only (build-linux-msan)'));
    assert(advisory.summary.includes(':warning: CI Advisory Report'));
    assert(advisory.summary.includes('**Failed jobs:** none'));
    assert(advisory.summary.includes('**Advisory lanes (continue-on-error):** linux-msan'));
    assert(advisory.summary.includes('use-of-uninitialized-value'));

    // Matrix lanes publish per-leg artifacts named after the needed job.
    const matrixAdvisory = await runScenario(
        'matrix-advisory',
        { 'build-macos': { result: 'success' }, 'build-macos-extra': { result: 'failure' } },
        { job: 'build-macos-Debug-metal', errors: ['ld: symbol not found'], warnings: [], tests: [] }
    );
    assert.strictEqual(matrixAdvisory.failed.length, 1, 'the failed needed job still fails the report');
    assert(matrixAdvisory.summary.includes('**Advisory lanes (continue-on-error):** macos-Debug-metal'));
    assert(matrixAdvisory.summary.includes("Required job 'build-macos-extra' concluded 'failure'"));

    // Advisory findings never mask a required failure in the same run.
    const mixed = await runScenario(
        'mixed',
        { 'build-linux-msan': { result: 'success' }, coverage: { result: 'failure' } },
        [msanArtifact, { job: 'coverage', errors: ['coverage.cpp:12:3: error: threshold failed'], warnings: [], tests: [] }]
    );
    assert.strictEqual(mixed.failed.length, 1);
    assert(mixed.failed[0].includes('2 errors'));
    assert(mixed.summary.includes(':x: CI Error Report'));
    assert(mixed.summary.includes('**Failed jobs:** coverage'));
    assert(mixed.summary.includes('**Advisory lanes (continue-on-error):** linux-msan'));

    // A summary from a job the workflow never handed over through `needs` is
    // unknown and fails closed.
    const unknownJob = await runScenario(
        'unknown-job',
        { 'build-linux-gcc': { result: 'success' } },
        { job: 'build-windows-shipping', errors: ['MSBuild error MSB1009'], warnings: [], tests: [] }
    );
    assert.strictEqual(unknownJob.failed.length, 1);
    assert(unknownJob.summary.includes('**Failed jobs:** windows-shipping'));

    // Warnings alone from a green lane are not findings.
    const warningsOnly = await runScenario(
        'warnings-only',
        { 'clang-tidy': { result: 'success' } },
        { job: 'clang-tidy', errors: [], warnings: ['a.cpp:1:1: warning: unused [misc]'], tests: [] }
    );
    assert.deepStrictEqual(warningsOnly.failed, []);
    assert(warningsOnly.info.includes('No errors to report'));

    console.log('report-ci-errors scenarios passed');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
