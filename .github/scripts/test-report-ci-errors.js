const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const reportCiErrors = require('./report-ci-errors.js');

async function runScenario(name, needs, artifact) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'spark-ci-report-'));
    const artifactDir = path.join(root, 'artifacts');
    const observed = { failed: [], info: [], warnings: [], summary: '' };

    try {
        if (artifact) {
            fs.mkdirSync(artifactDir, { recursive: true });
            fs.writeFileSync(
                path.join(artifactDir, 'error-summary.json'),
                JSON.stringify(artifact),
                'utf8'
            );
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

    console.log('report-ci-errors scenarios passed');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
