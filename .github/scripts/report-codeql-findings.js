// report-codeql-findings.js — trusted workflow_run CodeQL evidence reporter
//
// This module is executed only from the protected/default-branch checkout in
// codeql-report.yml. The source CodeQL workflow uploads raw SARIF and never
// executes this file. Downloaded artifacts are untrusted data and are bounded,
// structurally validated, escaped for Markdown, and tied to one exact run.

const fs = require('fs');
const path = require('path');

const COMMENT_MARKER = '<!-- spark-codeql-report -->';
const SOURCE_WORKFLOW_NAME = 'CodeQL Advanced';
const SOURCE_WORKFLOW_PATH = '.github/workflows/codeql.yml';
const ARTIFACT_FILES = Object.freeze({
    actions: 'actions.sarif',
    'c-cpp': 'cpp.sarif',
    python: 'python.sarif'
});
const SCHEMA_VERSION = 2;
const LANGUAGE_PATTERN = /^[A-Za-z0-9][A-Za-z0-9+._-]*$/;
const SHA_PATTERN = /^[0-9a-f]{40}$/i;
const DIGEST_PATTERN = /^sha256:[0-9a-f]{64}$/i;
const MAX_ARTIFACT_BYTES = 25 * 1024 * 1024;
const MAX_TOTAL_ARTIFACT_BYTES = 75 * 1024 * 1024;
const MAX_SARIF_BYTES = 25 * 1024 * 1024;
const MAX_TOTAL_SARIF_BYTES = 75 * 1024 * 1024;
const MAX_RESULTS_PER_FILE = 10000;
const MAX_TOTAL_RESULTS = 10000;
const MAX_RUNS_PER_FILE = 10;
const MAX_INVOCATIONS_PER_RUN = 20;
const MAX_NOTIFICATIONS_PER_RUN = 10000;
const MAX_ARTIFACTS = 100;
const MAX_SAME_COMMIT_RUNS = 1000;
const MAX_PULL_REQUEST_CANDIDATES = 100;
const MAX_COMMENT_PAGES = 5;
const MAX_RETAINED_FINDINGS = 300;
const MAX_SUMMARY_MESSAGES = 100;
const MAX_COMMENT_BYTES = 65000;

const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const normalizedSha = value => SHA_PATTERN.test(value || '') ? value.toLowerCase() : null;

function unique(values) {
    return [...new Set(values)];
}

function readEvent() {
    const eventPath = process.env.WORKFLOW_RUN_EVENT_PATH || process.env.GITHUB_EVENT_PATH;
    if (!eventPath) throw new Error('No workflow_run event path was provided.');
    const event = JSON.parse(fs.readFileSync(eventPath, 'utf8'));
    if (!isObject(event)) throw new Error('The workflow_run event payload is not an object.');
    return event;
}

function parseExpectedLanguages() {
    let parsed;
    try {
        parsed = JSON.parse(process.env.EXPECTED_LANGUAGES || '');
    } catch (error) {
        throw new Error(`EXPECTED_LANGUAGES is not valid JSON: ${error.message}`);
    }
    if (!Array.isArray(parsed) || parsed.length === 0 ||
        parsed.some(language => typeof language !== 'string' || !LANGUAGE_PATTERN.test(language) ||
            !Object.hasOwn(ARTIFACT_FILES, language))) {
        throw new Error('EXPECTED_LANGUAGES must be a non-empty JSON array of supported safe language names.');
    }
    if (new Set(parsed).size !== parsed.length) {
        throw new Error('EXPECTED_LANGUAGES contains duplicate language names.');
    }
    return parsed;
}

function responseItems(response, field) {
    // Octokit's paginate iterator normalizes wrapped list responses such as
    // { total_count, artifacts } to a root response.data array. Direct REST
    // calls retain the named wrapper, so accept only those two exact shapes.
    const value = Array.isArray(response?.data) ? response.data :
        field ? response?.data?.[field] : response?.data;
    if (!Array.isArray(value)) throw new Error(`API response field '${field || '<root>'}' is not an array`);
    return value;
}

async function paginateBounded(github, method, request, field, maxItems, label) {
    const perPage = Math.min(100, maxItems);
    const items = [];

    if (typeof github.paginate?.iterator === 'function') {
        for await (const response of github.paginate.iterator(method, { ...request, per_page: perPage })) {
            const page = responseItems(response, field);
            if (items.length + page.length > maxItems) throw new Error(`${label} exceeds the ${maxItems}-item limit`);
            items.push(...page);
        }
        return items;
    }

    for (let pageNumber = 1; items.length < maxItems; pageNumber += 1) {
        const response = await method({ ...request, per_page: perPage, page: pageNumber });
        const page = responseItems(response, field);
        if (items.length + page.length > maxItems) throw new Error(`${label} exceeds the ${maxItems}-item limit`);
        items.push(...page);
        if (page.length < perPage || !response?.headers?.link?.includes('rel="next"')) return items;
    }
    throw new Error(`${label} exceeds the ${maxItems}-item limit`);
}

function workflowPath(value) {
    return typeof value === 'string' ? value.split('@')[0] : '';
}

function artifactLabel(value) {
    return typeof value === 'string' && value ? value : '<invalid-artifact-name>';
}

function artifactManifest(inspection) {
    return inspection.expectedLanguages.map(language => {
        const name = ARTIFACT_FILES[language];
        const artifact = inspection.artifacts.find(candidate => candidate?.name === name);
        return {
            language,
            name,
            id: artifact?.id || null,
            size: artifact?.size_in_bytes ?? null,
            digest: artifact?.digest || null,
            workflowRun: isObject(artifact?.workflow_run) ? {
                id: artifact.workflow_run.id ?? null,
                repositoryId: artifact.workflow_run.repository_id ?? null,
                headRepositoryId: artifact.workflow_run.head_repository_id ?? null,
                headBranch: artifact.workflow_run.head_branch ?? null,
                headSha: artifact.workflow_run.head_sha ?? null
            } : null
        };
    });
}

function validateRepository(repository, owner, repo, expectedId, label, errors) {
    if (!isObject(repository) || repository.full_name !== `${owner}/${repo}` ||
        !Number.isInteger(repository.id) || repository.id < 1 ||
        (expectedId !== null && expectedId !== undefined && repository.id !== expectedId)) {
        errors.push(`${label} does not identify the expected repository.`);
        return false;
    }
    return true;
}

function pullMatchesSource(pull, repository, run) {
    return isObject(pull) && Number.isInteger(pull.number) && pull.number > 0 && pull.state === 'open' &&
        pull.base?.repo?.id === repository.id && pull.base.repo.full_name === repository.full_name &&
        pull.head?.repo?.id === run.head_repository.id &&
        pull.head.repo.full_name === run.head_repository.full_name &&
        normalizedSha(pull.head?.sha) === normalizedSha(run.head_sha);
}

async function resolvePullRequest(github, owner, repo, repository, run, eventRun, errors) {
    if (run.event !== 'pull_request') return { number: null, sourceHead: run.head_sha };

    const eventReferences = Array.isArray(eventRun.pull_requests) ? eventRun.pull_requests : [];
    const runReferences = Array.isArray(run.pull_requests) ? run.pull_requests : [];
    const references = [...eventReferences, ...runReferences]
        .filter(reference => isObject(reference));
    const numbers = unique(references
        .map(reference => reference.number)
        .filter(number => Number.isInteger(number) && number > 0));

    if (numbers.length === 1) {
        try {
            const pull = (await github.rest.pulls.get({ owner, repo, pull_number: numbers[0] })).data;
            if (pullMatchesSource(pull, repository, run)) {
                return { number: pull.number, sourceHead: normalizedSha(pull.head.sha) };
            }
        } catch {
            // Fork workflow_run payloads are frequently missing PR references;
            // fall through to the fork-owner/head-branch selector below.
        }
    }

    const [headOwner, headRepo, ...extra] = String(run.head_repository?.full_name || '').split('/');
    if (!headOwner || !headRepo || extra.length || !run.head_branch) {
        errors.push('The source head repository or branch cannot identify a pull request.');
        return { number: null, sourceHead: null };
    }

    try {
        const candidates = await paginateBounded(github, github.rest.pulls.list, {
            owner, repo, state: 'open', head: `${headOwner}:${run.head_branch}`
        }, null, MAX_PULL_REQUEST_CANDIDATES, 'pull-request candidate list');
        const exact = candidates.filter(pull => pullMatchesSource(pull, repository, run));
        if (exact.length === 1 && (numbers.length === 0 ||
            (numbers.length === 1 && numbers[0] === exact[0].number))) {
            return { number: exact[0].number, sourceHead: normalizedSha(exact[0].head.sha) };
        }
        errors.push(`Expected exactly one exact source pull request, found ${exact.length}.`);
    } catch (error) {
        errors.push(`Could not resolve the source pull request by fork head: ${error.message}`);
    }
    return { number: null, sourceHead: null };
}

function finalizeInspection(values) {
    const reporterTestOutcome = (process.env.REPORTER_TEST_OUTCOME || '').trim() || 'unknown';
    if (reporterTestOutcome !== 'success') {
        values.evidenceErrors.push(`Trusted reporter tests concluded '${reporterTestOutcome}'.`);
    }
    const inspection = {
        ...values,
        identityErrors: unique(values.identityErrors),
        evidenceErrors: unique(values.evidenceErrors),
        staleReasons: unique(values.staleReasons),
        trustedSource: values.identityErrors.length === 0
    };
    inspection.artifactManifest = artifactManifest(inspection);
    return inspection;
}

async function inspectSourceRun({ github, context }) {
    const owner = context.repo.owner;
    const repo = context.repo.repo;
    const identityErrors = [];
    const evidenceErrors = [];
    const staleReasons = [];
    let event = null;
    let eventRun = null;
    let run = null;
    let repository = null;
    let artifacts = [];
    let expectedLanguages = [];
    let pullRequest = { number: null, sourceHead: null };

    try { expectedLanguages = parseExpectedLanguages(); }
    catch (error) { identityErrors.push(error.message); }

    try {
        event = readEvent();
        eventRun = event.workflow_run;
    } catch (error) {
        identityErrors.push(`Could not read workflow_run event: ${error.message}`);
    }

    if (event) {
        if (event.action !== 'completed') identityErrors.push(`Unexpected workflow_run action '${event.action || 'unknown'}'.`);
        validateRepository(event.repository, owner, repo, null, 'Event repository', identityErrors);
    }
    if (!isObject(eventRun) || !Number.isInteger(eventRun.id) || eventRun.id < 1) {
        identityErrors.push('The event does not contain a valid source workflow run.');
    } else {
        if (!Number.isInteger(eventRun.workflow_id) || eventRun.workflow_id < 1) identityErrors.push('The event source workflow ID is invalid.');
        if (!Number.isInteger(eventRun.run_number) || eventRun.run_number < 1) identityErrors.push('The event source run number is invalid.');
        if (!Number.isInteger(eventRun.run_attempt) || eventRun.run_attempt < 1) identityErrors.push('The event source run attempt is invalid.');
        if (eventRun.name !== SOURCE_WORKFLOW_NAME) identityErrors.push(`Unexpected source workflow name '${eventRun.name || 'unknown'}'.`);
        if (workflowPath(eventRun.path) !== SOURCE_WORKFLOW_PATH) identityErrors.push(`Unexpected source workflow path '${eventRun.path || 'unknown'}'.`);
        if (!SHA_PATTERN.test(eventRun.head_sha || '')) identityErrors.push('The event source run has an invalid head SHA.');
        if (eventRun.status !== 'completed') identityErrors.push(`Unexpected event source status '${eventRun.status || 'unknown'}'.`);
        if (!['pull_request', 'push', 'schedule', 'workflow_dispatch'].includes(eventRun.event)) {
            identityErrors.push(`Unsupported event source type '${eventRun.event || 'unknown'}'.`);
        }
        if (typeof eventRun.head_branch !== 'string' || !eventRun.head_branch) {
            identityErrors.push('The event source run has no valid head branch.');
        }
        if (!isObject(eventRun.head_repository) || !Number.isInteger(eventRun.head_repository.id) ||
            eventRun.head_repository.id < 1 ||
            typeof eventRun.head_repository.full_name !== 'string' ||
            !/^[^/]+\/[^/]+$/.test(eventRun.head_repository.full_name)) {
            identityErrors.push('The event source head repository is invalid.');
        }
        validateRepository(eventRun.repository, owner, repo, event?.repository?.id, 'Source-run repository', identityErrors);
    }

    try {
        const response = await github.rest.repos.get({ owner, repo });
        repository = response.data;
        validateRepository(repository, owner, repo, event?.repository?.id, 'API repository', identityErrors);
        if (typeof repository.default_branch !== 'string' || !repository.default_branch) {
            identityErrors.push('The API repository has no valid default branch.');
        }
    } catch (error) {
        identityErrors.push(`Could not load trusted repository metadata: ${error.message}`);
    }

    if (isObject(eventRun) && Number.isInteger(eventRun.id)) {
        try {
            const response = await github.rest.actions.getWorkflowRun({ owner, repo, run_id: eventRun.id });
            run = response.data;
        } catch (error) {
            identityErrors.push(`Could not load the source workflow run: ${error.message}`);
        }
    }

    if (isObject(run)) {
        if (!Number.isInteger(run.id) || run.id < 1 || !Number.isInteger(run.workflow_id) || run.workflow_id < 1 ||
            !Number.isInteger(run.run_number) || run.run_number < 1 ||
            !Number.isInteger(run.run_attempt) || run.run_attempt < 1) {
            identityErrors.push('API source workflow run metadata is invalid.');
        }
        if (run.id !== eventRun.id || run.workflow_id !== eventRun.workflow_id ||
            run.run_number !== eventRun.run_number) {
            identityErrors.push('Source workflow run ID metadata does not match the event.');
        }
        if (run.name !== SOURCE_WORKFLOW_NAME || workflowPath(run.path) !== SOURCE_WORKFLOW_PATH) {
            identityErrors.push('API source workflow identity is not the trusted scanner workflow.');
        }
        if (run.status !== 'completed') evidenceErrors.push(`Source workflow status is '${run.status || 'unknown'}', not completed.`);
        if (run.status !== eventRun.status || run.event !== eventRun.event ||
            run.head_branch !== eventRun.head_branch) {
            identityErrors.push('API source workflow metadata does not match the completed event.');
        }
        if (run.conclusion !== eventRun.conclusion) staleReasons.push('The source workflow conclusion changed after this event.');
        if (run.run_attempt !== eventRun.run_attempt) staleReasons.push('A newer attempt exists for this source workflow run.');
        if (!normalizedSha(run.head_sha) || normalizedSha(run.head_sha) !== normalizedSha(eventRun.head_sha)) {
            staleReasons.push('The source workflow commit no longer matches the completed event.');
        }
        if (!['pull_request', 'push', 'schedule', 'workflow_dispatch'].includes(run.event)) {
            identityErrors.push(`Unsupported source workflow event '${run.event || 'unknown'}'.`);
        }
        validateRepository(run.repository, owner, repo, repository?.id, 'API source-run repository', identityErrors);
        if (!isObject(run.head_repository) || !Number.isInteger(run.head_repository.id) ||
            run.head_repository.id !== eventRun.head_repository?.id ||
            run.head_repository.full_name !== eventRun.head_repository?.full_name) {
            identityErrors.push('API source head repository does not match the completed event.');
        }
        if (run.conclusion !== 'success') evidenceErrors.push(`Source workflow concluded '${run.conclusion || 'unknown'}'.`);
    }

    // A PR may change its scanner workflow. Attest that the workflow blob used
    // by the source run is byte-identical to the current trusted default branch
    // before authorizing any artifact download.
    const [sourceOwner, sourceRepo, ...sourceRepoExtra] = String(run?.head_repository?.full_name || '').split('/');
    if (isObject(run) && isObject(repository) && SHA_PATTERN.test(run.head_sha || '') &&
        sourceOwner && sourceRepo && sourceRepoExtra.length === 0) {
        try {
            const [sourceContent, trustedContent] = await Promise.all([
                github.rest.repos.getContent({
                    owner: sourceOwner, repo: sourceRepo, path: SOURCE_WORKFLOW_PATH, ref: run.head_sha
                }),
                github.rest.repos.getContent({ owner, repo, path: SOURCE_WORKFLOW_PATH, ref: repository.default_branch })
            ]);
            const sourceFile = sourceContent.data;
            const trustedFile = trustedContent.data;
            if (!isObject(sourceFile) || !isObject(trustedFile) || sourceFile.type !== 'file' || trustedFile.type !== 'file' ||
                !SHA_PATTERN.test(sourceFile.sha || '') || !SHA_PATTERN.test(trustedFile.sha || '') ||
                sourceFile.sha.toLowerCase() !== trustedFile.sha.toLowerCase()) {
                identityErrors.push('Source scanner workflow does not match the trusted default-branch workflow.');
            }
        } catch (error) {
            identityErrors.push(`Could not attest the source scanner workflow: ${error.message}`);
        }
    } else if (isObject(run)) {
        identityErrors.push('The API source head repository cannot identify a workflow blob.');
    }

    // Once source identity fails, do not let an attacker amplify the privileged
    // workflow through a huge artifact inventory or unrelated PR lookups.
    if (identityErrors.length) {
        return finalizeInspection({
            owner, repo, event, eventRun, run, repository, expectedLanguages, artifacts, pullRequest,
            identityErrors, evidenceErrors, staleReasons
        });
    }

    if (isObject(run)) {
        try {
            artifacts = await paginateBounded(github, github.rest.actions.listWorkflowRunArtifacts, {
                owner, repo, run_id: run.id
            }, 'artifacts', MAX_ARTIFACTS, 'source artifact inventory');
        } catch (error) {
            evidenceErrors.push(`Could not list source-run artifacts: ${error.message}`);
            artifacts = [];
        }
    }

    const expectedNames = expectedLanguages.map(language => ARTIFACT_FILES[language]);
    const artifactCounts = new Map();
    let totalArtifactBytes = 0;
    for (const artifact of artifacts) {
        const name = artifactLabel(artifact?.name);
        artifactCounts.set(name, (artifactCounts.get(name) || 0) + 1);
        if (!expectedNames.includes(name)) evidenceErrors.push(`Unexpected source artifact '${name}'.`);
        if (!Number.isInteger(artifact?.id) || artifact.id < 1) evidenceErrors.push(`Artifact '${name}' has an invalid ID.`);
        if (artifact?.expired) evidenceErrors.push(`Artifact '${name}' is expired.`);
        if (!Number.isInteger(artifact?.size_in_bytes) || artifact.size_in_bytes < 0 ||
            artifact.size_in_bytes > MAX_ARTIFACT_BYTES) {
            evidenceErrors.push(`Artifact '${name}' has an invalid or excessive size.`);
        } else totalArtifactBytes += artifact.size_in_bytes;
        if (!DIGEST_PATTERN.test(artifact?.digest || '')) evidenceErrors.push(`Artifact '${name}' has no valid SHA-256 digest.`);
        const provenance = artifact?.workflow_run;
        if (!isObject(provenance) || provenance.id !== run?.id ||
            provenance.repository_id !== repository?.id ||
            provenance.head_repository_id !== run?.head_repository?.id ||
            provenance.head_branch !== run?.head_branch ||
            normalizedSha(provenance.head_sha) !== normalizedSha(run?.head_sha)) {
            evidenceErrors.push(`Artifact '${name}' is not tied to the exact source run and commit.`);
        }
    }
    if (totalArtifactBytes > MAX_TOTAL_ARTIFACT_BYTES) evidenceErrors.push('Source artifacts exceed the total size limit.');
    for (const name of expectedNames) {
        const count = artifactCounts.get(name) || 0;
        if (count !== 1) evidenceErrors.push(`Expected exactly one '${name}' artifact, found ${count}.`);
    }

    if (isObject(run)) {
        pullRequest = await resolvePullRequest(github, owner, repo, repository, run, eventRun, evidenceErrors);
    }

    return finalizeInspection({
        owner, repo, event, eventRun, run, repository, expectedLanguages, artifacts, pullRequest,
        identityErrors, evidenceErrors, staleReasons
    });
}

function normalizeLevel(level) {
    return ['error', 'warning', 'note', 'none'].includes(level) ? level : 'warning';
}

function deduplicateFindings(findings) {
    const uniqueFindings = new Map();
    for (const finding of findings) {
        const key = JSON.stringify([finding.tool, finding.rule, finding.file, finding.line, finding.message]);
        if (!uniqueFindings.has(key)) {
            uniqueFindings.set(key, { ...finding, languages: new Set([finding.language]) });
        } else uniqueFindings.get(key).languages.add(finding.language);
    }
    const severity = { error: 0, warning: 1, note: 2, none: 3 };
    return [...uniqueFindings.values()]
        .map(finding => ({ ...finding, languages: [...finding.languages].sort() }))
        .sort((left, right) => severity[left.level] - severity[right.level] ||
            left.file.localeCompare(right.file) || left.line - right.line ||
            left.rule.localeCompare(right.rule) || left.message.localeCompare(right.message));
}

function parseSarif(filePath, language) {
    const stat = fs.statSync(filePath);
    if (!stat.isFile() || stat.size > MAX_SARIF_BYTES) throw new Error('SARIF file has an invalid or excessive size');
    let sarif;
    try { sarif = JSON.parse(fs.readFileSync(filePath, 'utf8')); }
    catch (error) { throw new Error(`invalid JSON: ${error.message}`); }
    if (!isObject(sarif) || sarif.version !== '2.1.0' ||
        !Array.isArray(sarif.runs) || sarif.runs.length === 0) {
        throw new Error('expected a SARIF 2.1.0 object with at least one run');
    }
    if (sarif.runs.length > MAX_RUNS_PER_FILE) throw new Error('SARIF file exceeds the run limit');

    const findings = [];
    let rawResults = 0;
    for (const [runIndex, run] of sarif.runs.entries()) {
        if (!isObject(run) || !isObject(run.tool) || !isObject(run.tool.driver)) {
            throw new Error(`run ${runIndex} is missing tool.driver metadata`);
        }
        const driver = run.tool.driver;
        const rules = driver.rules === undefined ? [] : driver.rules;
        const results = run.results === undefined ? [] : run.results;
        const invocations = run.invocations;
        if (driver.name !== 'CodeQL' || !Array.isArray(rules) || !Array.isArray(results)) {
            throw new Error(`run ${runIndex} has malformed tool, rules, or results metadata`);
        }
        const automationId = run.automationDetails?.id;
        if (automationId !== `/language:${language}` && automationId !== `/language:${language}/`) {
            throw new Error(`run ${runIndex} does not attest the expected '${language}' language category`);
        }
        if (rules.length > MAX_RESULTS_PER_FILE) throw new Error(`run ${runIndex} exceeds the rule limit`);
        if (results.length > MAX_RESULTS_PER_FILE) throw new Error(`run ${runIndex} exceeds the result limit`);
        if (!Array.isArray(invocations) || invocations.length === 0) {
            throw new Error(`run ${runIndex} has no invocation evidence`);
        }
        if (invocations.length > MAX_INVOCATIONS_PER_RUN) {
            throw new Error(`run ${runIndex} exceeds the invocation limit`);
        }

        let notificationCount = 0;
        for (const [invocationIndex, invocation] of invocations.entries()) {
            if (!isObject(invocation)) {
                throw new Error(`run ${runIndex} invocation ${invocationIndex} is not an object`);
            }
            if (invocation.executionSuccessful !== true) {
                throw new Error(`run ${runIndex} invocation ${invocationIndex} did not complete successfully`);
            }
            for (const field of ['toolExecutionNotifications', 'configurationNotifications']) {
                const notifications = invocation[field] === undefined ? [] : invocation[field];
                if (!Array.isArray(notifications)) {
                    throw new Error(`run ${runIndex} invocation ${invocationIndex} has malformed ${field}`);
                }
                notificationCount += notifications.length;
                if (notificationCount > MAX_NOTIFICATIONS_PER_RUN) {
                    throw new Error(`run ${runIndex} exceeds the notification limit`);
                }
                for (const [notificationIndex, notification] of notifications.entries()) {
                    if (!isObject(notification)) {
                        throw new Error(
                            `run ${runIndex} invocation ${invocationIndex} ${field}[${notificationIndex}] is not an object`
                        );
                    }
                    if (notification.level !== undefined &&
                        !['error', 'warning', 'note', 'none'].includes(notification.level)) {
                        throw new Error(
                            `run ${runIndex} invocation ${invocationIndex} ${field}[${notificationIndex}] has an invalid level`
                        );
                    }
                    if (notification.level === 'error') {
                        const identifier = typeof notification.descriptor?.id === 'string'
                            ? notification.descriptor.id.slice(0, 200) : 'unknown';
                        const message = typeof notification.message?.text === 'string'
                            ? notification.message.text.slice(0, 300) : 'CodeQL reported an execution error';
                        throw new Error(
                            `run ${runIndex} reports CodeQL error notification '${identifier}': ${message}`
                        );
                    }
                }
            }
        }

        const rulesByKey = new Map();
        rules.forEach((rule, index) => {
            if (!isObject(rule)) throw new Error(`run ${runIndex} rule ${index} is not an object`);
            if (rule.id !== undefined && typeof rule.id !== 'string') {
                throw new Error(`run ${runIndex} rule ${index} has an invalid ID`);
            }
            if (rule.defaultConfiguration?.level !== undefined &&
                !['error', 'warning', 'note', 'none'].includes(rule.defaultConfiguration.level)) {
                throw new Error(`run ${runIndex} rule ${index} has an invalid level`);
            }
            if (typeof rule.id === 'string') rulesByKey.set(rule.id, rule);
            rulesByKey.set(String(index), rule);
        });

        for (const [resultIndex, result] of results.entries()) {
            if (!isObject(result)) throw new Error(`run ${runIndex} result ${resultIndex} is not an object`);
            if (!isObject(result.message) ||
                (typeof result.message.text !== 'string' && typeof result.message.markdown !== 'string')) {
                throw new Error(`run ${runIndex} result ${resultIndex} has no valid message`);
            }
            if (result.level !== undefined && !['error', 'warning', 'note', 'none'].includes(result.level)) {
                throw new Error(`run ${runIndex} result ${resultIndex} has an invalid level`);
            }
            rawResults += 1;
            const ruleId = typeof result.ruleId === 'string' ? result.ruleId :
                (typeof result.rule?.id === 'string' ? result.rule.id : 'unknown');
            const rule = rulesByKey.get(ruleId) ||
                (Number.isInteger(result.ruleIndex) ? rulesByKey.get(String(result.ruleIndex)) : undefined) || {};
            const physical = result.locations?.[0]?.physicalLocation;
            const message = result.message?.text || result.message?.markdown || rule.shortDescription?.text || '';
            findings.push({
                tool: driver.name.slice(0, 100),
                rule: ruleId.slice(0, 200),
                ruleName: String(rule.name || rule.shortDescription?.text || ruleId).slice(0, 200),
                level: normalizeLevel(result.level || rule.defaultConfiguration?.level),
                file: typeof physical?.artifactLocation?.uri === 'string' ? physical.artifactLocation.uri.slice(0, 500) : '',
                line: Number.isInteger(physical?.region?.startLine) && physical.region.startLine > 0
                    ? physical.region.startLine : 0,
                message: String(message).slice(0, 500),
                language
            });
        }
    }
    return { findings, rawResults, bytes: stat.size };
}

function parseDownloadedArtifacts(artifactRoot, expectedLanguages) {
    const errors = [];
    const findings = [];
    const completedLanguages = [];
    const invalidLanguages = new Set();
    const missingLanguages = [];
    const unexpectedArtifacts = [];
    let rawResults = 0;
    let totalBytes = 0;

    if (!fs.existsSync(artifactRoot) || !fs.statSync(artifactRoot).isDirectory()) {
        errors.push(`Downloaded artifact directory '${artifactRoot}' does not exist.`);
        return { errors, findings, completedLanguages, invalidLanguages: [], missingLanguages: [...expectedLanguages], unexpectedArtifacts, rawResults };
    }

    const entries = fs.readdirSync(artifactRoot, { withFileTypes: true });
    const files = new Map();
    for (const entry of entries) {
        const entryPath = path.join(artifactRoot, entry.name);
        if (!entry.isFile() || entry.isSymbolicLink()) {
            errors.push(`Unexpected or non-regular downloaded artifact '${entry.name}'.`);
            unexpectedArtifacts.push(entry.name);
        } else files.set(entry.name, entryPath);
    }

    const expectedNames = new Set(expectedLanguages.map(language => ARTIFACT_FILES[language]));
    for (const name of files.keys()) {
        if (!expectedNames.has(name)) {
            unexpectedArtifacts.push(name);
            errors.push(`Unexpected downloaded artifact '${name}'.`);
        }
    }

    for (const language of expectedLanguages) {
        const name = ARTIFACT_FILES[language];
        const file = files.get(name);
        if (!file) {
            missingLanguages.push(language);
            errors.push(`Downloaded artifact '${name}' is missing.`);
            continue;
        }

        let languageValid = true;
        try {
            const parsed = parseSarif(file, language);
            totalBytes += parsed.bytes;
            if (totalBytes > MAX_TOTAL_SARIF_BYTES) throw new Error('downloaded SARIF exceeds the total size limit');
            if (rawResults + parsed.rawResults > MAX_TOTAL_RESULTS) throw new Error('downloaded SARIF exceeds the total result limit');
            rawResults += parsed.rawResults;
            findings.push(...parsed.findings);
        } catch (error) {
            errors.push(`Invalid SARIF in '${name}': ${error.message}`);
            languageValid = false;
        }
        if (languageValid) completedLanguages.push(language);
        else invalidLanguages.add(language);
    }

    return {
        errors: unique(errors),
        findings,
        completedLanguages,
        invalidLanguages: [...invalidLanguages].sort(),
        missingLanguages,
        unexpectedArtifacts: unique(unexpectedArtifacts),
        rawResults
    };
}

function findingCounts(findings) {
    const counts = { error: 0, warning: 0, note: 0 };
    for (const finding of findings) {
        if (finding.level === 'error') counts.error += 1;
        else if (finding.level === 'warning') counts.warning += 1;
        else counts.note += 1;
    }
    return counts;
}

function buildSummary(inspection, parsed, evidenceErrors, status) {
    const allFindings = deduplicateFindings(parsed.findings || []);
    const findings = allFindings.slice(0, MAX_RETAINED_FINDINGS);
    const counts = findingCounts(allFindings);
    const run = inspection.run || inspection.eventRun || {};
    return {
        schemaVersion: SCHEMA_VERSION,
        kind: 'codeql-trusted-workflow-run',
        generatedAt: new Date().toISOString(),
        status,
        source: {
            repository: `${inspection.owner}/${inspection.repo}`,
            workflowName: run.name || null,
            workflowPath: workflowPath(run.path) || null,
            workflowId: run.workflow_id || null,
            runId: run.id || null,
            runNumber: run.run_number || null,
            runAttempt: inspection.eventRun?.run_attempt || null,
            event: run.event || null,
            conclusion: run.conclusion || null,
            analysisCommit: SHA_PATTERN.test(run.head_sha || '') ? run.head_sha.toLowerCase() : null,
            pullRequest: inspection.pullRequest.number,
            sourceHeadCommit: inspection.pullRequest.sourceHead,
            headRepository: run.head_repository?.full_name || null,
            headRepositoryId: run.head_repository?.id || null,
            headBranch: run.head_branch || null
        },
        artifactEvidence: inspection.artifactManifest,
        expectedLanguages: inspection.expectedLanguages,
        completedLanguages: parsed.completedLanguages || [],
        missingLanguages: parsed.missingLanguages || [],
        invalidLanguages: parsed.invalidLanguages || [],
        unexpectedArtifacts: parsed.unexpectedArtifacts || [],
        rawResults: parsed.rawResults || 0,
        uniqueFindings: allFindings.length,
        retainedFindings: findings.length,
        omittedFindings: allFindings.length - findings.length,
        counts,
        findings,
        evidenceErrors: unique(evidenceErrors),
        staleReasons: [...inspection.staleReasons],
        mutation: { target: inspection.pullRequest.number ? 'pull-request-comment' : 'job-summary', performed: false, reason: 'pending' },
        jobSummary: { performed: false },
        errors: findings.filter(finding => finding.level === 'error')
            .slice(0, MAX_SUMMARY_MESSAGES)
            .map(finding => `${finding.file}:${finding.line} [${finding.rule}] ${finding.message}`),
        warnings: findings.filter(finding => finding.level === 'warning')
            .slice(0, MAX_SUMMARY_MESSAGES)
            .map(finding => `${finding.file}:${finding.line} [${finding.rule}] ${finding.message}`),
        tests: []
    };
}

function writeSummary(summary, core) {
    const summaryPath = process.env.SUMMARY_PATH || 'codeql-summary.json';
    fs.mkdirSync(path.dirname(path.resolve(summaryPath)), { recursive: true });
    fs.writeFileSync(summaryPath, `${JSON.stringify(summary, null, 2)}\n`, 'utf8');
    core.info(`Wrote ${summaryPath}`);
}

const escapeMarkdown = value => String(value)
    .replace(/[\r\n]+/g, ' ')
    .replace(/([\\`*_{}\[\]()#+\-.!|~])/g, '\\$1')
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/@/g, '&#64;').replace(/:/g, '&#58;');

function reportStateMarker(summary) {
    const source = summary.source;
    return `<!-- spark-codeql-report-state run-number=${source.runNumber} run-attempt=${source.runAttempt} ` +
        `run-id=${source.runId} pr-head=${source.sourceHeadCommit || 'none'} run-head=${source.analysisCommit || 'none'} -->`;
}

function commentSourceState(comment) {
    const match = comment?.body?.match(/<!-- spark-codeql-report-state run-number=(\d+) run-attempt=(\d+) run-id=(\d+) pr-head=([0-9a-f]{40}|none) run-head=([0-9a-f]{40}|none) -->/i);
    if (!match) return null;
    return {
        runNumber: Number(match[1]),
        runAttempt: Number(match[2]),
        runId: Number(match[3]),
        sourceHead: match[4].toLowerCase(),
        runHead: match[5].toLowerCase()
    };
}

function commentIsNewer(comment, summary) {
    const state = commentSourceState(comment);
    if (!state) return false;
    return state.runNumber > summary.source.runNumber ||
        (state.runNumber === summary.source.runNumber && state.runAttempt > summary.source.runAttempt);
}

function codeList(values) {
    return values.length ? values.map(value => `\`${escapeMarkdown(value)}\``).join(', ') : '_none_';
}

function truncateUtf8(value, maxBytes) {
    if (maxBytes <= 0) return '';
    if (Buffer.byteLength(value, 'utf8') <= maxBytes) return value;
    let bytes = 0;
    let truncated = '';
    for (const character of value) {
        const characterBytes = Buffer.byteLength(character, 'utf8');
        if (bytes + characterBytes > maxBytes) break;
        truncated += character;
        bytes += characterBytes;
    }
    return truncated;
}

function finishComment(lines, footerLines) {
    const footer = `\n${footerLines.join('\n')}`;
    const truncationNotice = '\n\n_Report body truncated; counts and provenance below remain authoritative._';
    let body = lines.join('\n');
    const budget = MAX_COMMENT_BYTES - Buffer.byteLength(footer, 'utf8');
    if (Buffer.byteLength(body, 'utf8') > budget) {
        const contentBudget = budget - Buffer.byteLength(truncationNotice, 'utf8');
        body = `${truncateUtf8(body, contentBudget)}${truncationNotice}`;
    }
    return `${body}${footer}`;
}

function reportBody(summary) {
    const lines = [COMMENT_MARKER, reportStateMarker(summary)];
    if (summary.status === 'stale') {
        lines.push('## :fast_forward: Stale CodeQL Report Skipped', '',
            'This completed run no longer targets the current pull-request head and did not mutate the PR.', '');
    } else if (summary.status === 'incomplete') {
        lines.push('## :x: CodeQL Evidence Incomplete', '',
            'This run is **not a clean scan** because trusted evidence is missing or invalid.', '',
            `**Expected languages:** ${codeList(summary.expectedLanguages)}`,
            `**Completed languages:** ${codeList(summary.completedLanguages)}`);
        if (summary.missingLanguages.length) lines.push(`**Missing languages:** ${codeList(summary.missingLanguages)}`);
        if (summary.invalidLanguages.length) lines.push(`**Invalid languages:** ${codeList(summary.invalidLanguages)}`);
        if (summary.unexpectedArtifacts.length) lines.push(`**Unexpected artifacts:** ${codeList(summary.unexpectedArtifacts)}`);
        lines.push('', '### Evidence errors', '');
        summary.evidenceErrors.slice(0, 30).forEach(error => lines.push(`- ${escapeMarkdown(String(error).slice(0, 500))}`));
        if (summary.evidenceErrors.length > 30) lines.push(`- _${summary.evidenceErrors.length - 30} additional errors omitted_`);
    } else if (summary.uniqueFindings === 0) {
        lines.push('## :white_check_mark: CodeQL Report', '',
            'All expected CodeQL analyses completed with valid SARIF and no findings.', '',
            `**Languages:** ${codeList(summary.completedLanguages)}`,
            '**Errors:** 0 | **Warnings:** 0 | **Notes:** 0');
    } else {
        lines.push('## :mag: CodeQL Report', '',
            `**Languages:** ${codeList(summary.completedLanguages)}`,
            `**Errors:** ${summary.counts.error} | **Warnings:** ${summary.counts.warning} | **Notes:** ${summary.counts.note}`, '');
        for (const [level, label, icon] of [['error', 'Errors', ':red_circle:'], ['warning', 'Warnings', ':warning:'], ['note', 'Notes', ':information_source:']]) {
            const matches = summary.findings.filter(finding => level === 'note'
                ? ['note', 'none'].includes(finding.level) : finding.level === level);
            const total = level === 'note' ? summary.counts.note : summary.counts[level];
            if (!total) continue;
            lines.push(`<details${level === 'error' ? ' open' : ''}><summary>${icon} ${label} (${total})</summary>`, '',
                '| Rule | Languages | Location | Message |', '|------|-----------|----------|---------|');
            for (const finding of matches.slice(0, 10)) {
                const location = finding.file ? `\`${escapeMarkdown(finding.file.slice(0, 220))}:${finding.line}\`` : '-';
                lines.push(`| \`${escapeMarkdown(finding.rule.slice(0, 120))}\` | ${finding.languages.map(escapeMarkdown).join(', ')} | ${location} | ${escapeMarkdown(finding.message.slice(0, 300))} |`);
            }
            if (total > Math.min(matches.length, 10)) {
                lines.push(`| ... | ... | ... | _${total - Math.min(matches.length, 10)} more omitted_ |`);
            }
            lines.push('', '</details>', '');
        }
    }
    const footer = [];
    if (summary.source.analysisCommit) footer.push('', `**Analyzed commit:** \`${summary.source.analysisCommit}\``);
    footer.push('', `*Updated: ${summary.generatedAt} — this comment is updated in-place.*`);
    return finishComment(lines, footer);
}

function isTrustedMarkerComment(comment) {
    if (comment?.body?.split(/\r?\n/, 1)[0] !== COMMENT_MARKER || comment.user?.type !== 'Bot') return false;
    // GitHub does not expose the originating workflow on issue comments. This
    // proves GitHub Actions app ownership, but not exclusive ownership by this
    // workflow; a dedicated App or user-managed secret would be required for that.
    return comment.user.login === 'github-actions[bot]' || comment.performed_via_github_app?.slug === 'github-actions';
}

function lastPageFromLink(link) {
    if (typeof link !== 'string') return 1;
    const match = link.match(/[?&]page=(\d+)[^>]*>;\s*rel="last"/);
    const page = match ? Number(match[1]) : 1;
    return Number.isSafeInteger(page) && page > 0 ? page : 1;
}

async function listBoundedComments(github, request) {
    const method = github.rest.issues.listComments;
    const firstResponse = await method({ ...request, per_page: 100, page: 1 });
    const comments = [...responseItems(firstResponse, null)];
    const lastPage = lastPageFromLink(firstResponse?.headers?.link);
    const pages = new Set([1]);
    for (let page = Math.max(2, lastPage - (MAX_COMMENT_PAGES - 2)); page <= lastPage; page += 1) pages.add(page);
    for (const page of [...pages].sort((left, right) => left - right)) {
        if (page === 1) continue;
        const response = await method({ ...request, per_page: 100, page });
        comments.push(...responseItems(response, null));
    }
    const byId = new Map();
    for (const comment of comments) {
        if (Number.isInteger(comment?.id) && comment.id > 0) byId.set(comment.id, comment);
    }
    return { comments: [...byId.values()], truncated: lastPage > MAX_COMMENT_PAGES };
}

async function writeJobSummary(core, summary) {
    await core.summary.addRaw(reportBody(summary).replace(COMMENT_MARKER, '').trim()).write();
    summary.jobSummary = { performed: true };
    if (summary.mutation.reason === 'pending' && summary.mutation.target === 'job-summary') {
        summary.mutation = { target: 'job-summary', performed: true, reason: 'written' };
    }
}

function markStale(summary, reasons) {
    if (summary.status === 'complete') summary.status = 'stale';
    summary.staleReasons = unique([...summary.staleReasons, ...reasons]);
    summary.mutation = { target: 'pull-request-comment', performed: false, reason: 'stale-source-run' };
}

async function preflight(args) {
    const inspection = await inspectSourceRun(args);
    const errors = [...inspection.identityErrors, ...inspection.evidenceErrors];
    const authorized = inspection.trustedSource && errors.length === 0 && inspection.staleReasons.length === 0;
    args.core.setOutput('authorized', authorized ? 'true' : 'false');
    args.core.setOutput('source-run-id', String(inspection.run?.id || ''));
    args.core.setOutput('artifact-ids', authorized
        ? inspection.artifactManifest.map(artifact => artifact.id).join(',') : '');
    args.core.setOutput('artifact-manifest', authorized
        ? JSON.stringify(inspection.artifactManifest) : '[]');
    if (!authorized) args.core.setFailed(`CodeQL source artifact preflight failed: ${[...errors, ...inspection.staleReasons].join(' ')}`);
    else args.core.info(`Authorized ${inspection.artifacts.length} immutable artifact(s) from source run ${inspection.run.id}.`);
    return inspection;
}

async function trustedReport(args) {
    const { github, context, core } = args;
    const inspection = await inspectSourceRun(args);
    const evidenceErrors = [...inspection.identityErrors, ...inspection.evidenceErrors];
    const preflightOutcome = (process.env.PREFLIGHT_OUTCOME || '').trim() || 'unknown';
    const downloadOutcome = (process.env.DOWNLOAD_OUTCOME || '').trim() || 'unknown';
    if (preflightOutcome !== 'success') evidenceErrors.push(`Trusted artifact preflight concluded '${preflightOutcome}'.`);
    if (downloadOutcome !== 'success') evidenceErrors.push(`Exact-run artifact download concluded '${downloadOutcome}'.`);
    try {
        const preflightManifest = JSON.parse(process.env.PREFLIGHT_ARTIFACT_MANIFEST || '');
        if (JSON.stringify(preflightManifest) !== JSON.stringify(inspection.artifactManifest)) {
            evidenceErrors.push('The source artifact manifest changed after trusted preflight.');
        }
    } catch (error) {
        evidenceErrors.push(`The trusted preflight artifact manifest is invalid: ${error.message}`);
    }

    let parsed = {
        errors: [], findings: [], completedLanguages: [], invalidLanguages: [],
        missingLanguages: [...inspection.expectedLanguages], unexpectedArtifacts: [], rawResults: 0
    };
    if (downloadOutcome === 'success') {
        parsed = parseDownloadedArtifacts(process.env.ARTIFACT_DIR || '', inspection.expectedLanguages);
        evidenceErrors.push(...parsed.errors);
    }

    const initialStatus = evidenceErrors.length ? 'incomplete' :
        (inspection.staleReasons.length ? 'stale' : 'complete');
    const summary = buildSummary(inspection, parsed, unique(evidenceErrors), initialStatus);

    const reporterTestsPassed = (process.env.REPORTER_TEST_OUTCOME || '').trim() === 'success';
    const isPullRequest = inspection.run?.event === 'pull_request';
    if (!isPullRequest) {
        if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
        await writeJobSummary(core, summary);
        writeSummary(summary, core);
        return summary;
    }

    if (!inspection.trustedSource || !reporterTestsPassed ||
        !inspection.pullRequest.number || !SHA_PATTERN.test(inspection.pullRequest.sourceHead || '')) {
        summary.mutation = { target: 'pull-request-comment', performed: false, reason: 'untrusted-or-unresolved-source' };
        if (summary.status !== 'stale') core.setFailed('CodeQL report could not safely resolve a trusted PR target.');
        await writeJobSummary(core, summary);
        writeSummary(summary, core);
        return summary;
    }

    if (inspection.staleReasons.length) {
        markStale(summary, inspection.staleReasons);
        if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
        await writeJobSummary(core, summary);
        writeSummary(summary, core);
        return summary;
    }

    try {
        // Resolve the owned marker first. The current source run and PR head are
        // then fetched immediately before the mutation so an older completion
        // cannot overwrite evidence for a newer attempt or commit.
        const commentListing = await listBoundedComments(github, {
            owner: inspection.owner,
            repo: inspection.repo,
            issue_number: inspection.pullRequest.number
        });
        if (commentListing.truncated) core.warning('PR comment search was bounded to the oldest and newest pages.');
        const existing = commentListing.comments.filter(isTrustedMarkerComment)
            .sort((left, right) => (right.id || 0) - (left.id || 0))[0] || null;

        const latestRun = (await github.rest.actions.getWorkflowRun({
            owner: inspection.owner, repo: inspection.repo, run_id: inspection.run.id
        })).data;
        if (latestRun.id !== inspection.run.id ||
            latestRun.workflow_id !== inspection.run.workflow_id ||
            latestRun.run_number !== inspection.run.run_number ||
            latestRun.run_attempt !== inspection.eventRun.run_attempt ||
            normalizedSha(latestRun.head_sha) !== normalizedSha(inspection.run.head_sha) ||
            latestRun.status !== 'completed' || latestRun.conclusion !== inspection.run.conclusion) {
            markStale(summary, ['The source workflow changed immediately before report mutation.']);
            if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
            await writeJobSummary(core, summary);
            writeSummary(summary, core);
            return summary;
        }

        const sameCommitRuns = await paginateBounded(github, github.rest.actions.listWorkflowRuns, {
            owner: inspection.owner,
            repo: inspection.repo,
            workflow_id: inspection.run.workflow_id,
            event: inspection.run.event,
            head_sha: inspection.run.head_sha
        }, 'workflow_runs', MAX_SAME_COMMIT_RUNS, 'same-commit workflow runs');
        if (sameCommitRuns.some(candidate => candidate.id !== inspection.run.id &&
            Number.isInteger(candidate.run_number) && candidate.run_number > inspection.run.run_number)) {
            markStale(summary, ['A newer source workflow run exists for the analyzed commit.']);
            if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
            await writeJobSummary(core, summary);
            writeSummary(summary, core);
            return summary;
        }

        const currentPull = (await github.rest.pulls.get({
            owner: inspection.owner, repo: inspection.repo, pull_number: inspection.pullRequest.number
        })).data;
        if (currentPull.state !== 'open' || currentPull.base?.repo?.full_name !== `${inspection.owner}/${inspection.repo}` ||
            currentPull.base?.repo?.id !== inspection.repository.id ||
            currentPull.head?.repo?.id !== inspection.run.head_repository?.id ||
            normalizedSha(currentPull.head?.sha) !== normalizedSha(inspection.pullRequest.sourceHead) ||
            normalizedSha(currentPull.head?.sha) !== normalizedSha(inspection.run.head_sha)) {
            markStale(summary, ['The pull-request head changed before report mutation.']);
            if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
            await writeJobSummary(core, summary);
            writeSummary(summary, core);
            return summary;
        }

        if (existing && commentIsNewer(existing, summary)) {
            markStale(summary, ['The owned CodeQL report already records a newer source run.']);
            if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
            await writeJobSummary(core, summary);
            writeSummary(summary, core);
            return summary;
        }

        const body = reportBody(summary);
        const withoutTimestamp = value => value.replace(/\*Updated:.*— this comment is updated in-place\.\*/, '');
        if (existing && withoutTimestamp(existing.body) === withoutTimestamp(body)) {
            summary.mutation = { target: 'pull-request-comment', performed: false, reason: 'unchanged' };
            core.info('Trusted CodeQL report is unchanged.');
        } else if (existing) {
            await github.rest.issues.updateComment({
                owner: inspection.owner, repo: inspection.repo, comment_id: existing.id, body
            });
            summary.mutation = { target: 'pull-request-comment', performed: true, reason: 'updated', commentId: existing.id };
        } else {
            const created = await github.rest.issues.createComment({
                owner: inspection.owner, repo: inspection.repo,
                issue_number: inspection.pullRequest.number, body
            });
            summary.mutation = {
                target: 'pull-request-comment', performed: true, reason: 'created',
                commentId: created.data?.id || null
            };
        }
    } catch (error) {
        summary.status = 'incomplete';
        summary.evidenceErrors = unique([...summary.evidenceErrors, `Trusted PR mutation failed: ${error.message}`]);
        summary.mutation = { target: 'pull-request-comment', performed: false, reason: 'api-error' };
        core.setFailed(`Trusted CodeQL PR mutation failed: ${error.message}`);
        await writeJobSummary(core, summary);
    }

    if (summary.status === 'incomplete') core.setFailed('CodeQL trusted evidence is incomplete.');
    writeSummary(summary, core);
    return summary;
}

module.exports = async args => {
    const mode = (process.env.REPORT_MODE || '').trim();
    if (mode === 'trusted-preflight') return preflight(args);
    if (mode === 'trusted-report') return trustedReport(args);
    throw new Error(`Untrusted REPORT_MODE '${mode || '<empty>'}' is not permitted.`);
};

module.exports._test = Object.freeze({ finishComment, parseSarif });
