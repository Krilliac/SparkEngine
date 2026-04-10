// report-ci-errors.js — Consolidated, deduplicated CI error reporter
//
// Called by the report-ci-errors job after all build jobs complete.
// Downloads error summary artifacts from each failed job, consolidates
// them into one PR comment (on PRs) or a job summary (on push).
//
// Strategy:
//   1. Collect error summaries from all failed job artifacts
//   2. Merge and deduplicate into one consolidated report
//   3. On PRs: find or create a single "CI Error Report" comment
//   4. On push: write a job summary visible in the Actions UI
//   5. On success (no errors), update/clear the old error comment if it exists

const COMMENT_MARKER = '<!-- spark-ci-error-report -->';

module.exports = async ({ github, context, core, glob, io, artifact }) => {
    const owner = context.repo.owner;
    const repo = context.repo.repo;
    const prNumber = context.issue.number;
    const isPullRequest = !!prNumber;

    // ── 1. Collect error summaries from downloaded artifacts ──────────
    const fs = require('fs');
    const path = require('path');

    const artifactDir = process.env.ARTIFACT_DIR || 'ci-error-summaries';
    const jobResults = [];

    if (fs.existsSync(artifactDir)) {
        const entries = fs.readdirSync(artifactDir, { recursive: true });
        for (const entry of entries) {
            const fullPath = path.join(artifactDir, entry);
            if (!fs.statSync(fullPath).isFile()) continue;

            try {
                const content = fs.readFileSync(fullPath, 'utf8').trim();
                if (!content) continue;

                // Try JSON format first
                try {
                    const data = JSON.parse(content);
                    if (data.job) jobResults.push(data);
                } catch {
                    // Fallback: parse text format
                    const lines = content.split('\n');
                    const jobLine = lines.find(l => l.startsWith('JOB:'));
                    if (!jobLine) continue;

                    const job = jobLine.replace('JOB:', '');
                    const sections = { errors: [], warnings: [], tests: [] };
                    let current = null;

                    for (const line of lines) {
                        if (line === '---ERRORS---') { current = 'errors'; continue; }
                        if (line === '---WARNINGS---') { current = 'warnings'; continue; }
                        if (line === '---TESTS---') { current = 'tests'; continue; }
                        if (current && line.trim()) sections[current].push(line);
                    }

                    jobResults.push({ job, ...sections });
                }
            } catch (e) {
                core.warning(`Failed to parse ${fullPath}: ${e.message}`);
            }
        }
    }

    // ── 2. Find existing error report comment (PRs only) ──────────────
    let existingComment = null;
    if (isPullRequest) {
        const comments = await github.rest.issues.listComments({
            owner, repo, issue_number: prNumber, per_page: 100
        });

        for (const comment of comments.data) {
            if (comment.body && comment.body.includes(COMMENT_MARKER)) {
                existingComment = comment;
                break;
            }
        }
    }

    // ── 3. If no errors found, clean up old comment ──────────────────
    const hasIssues = jobResults.some(
        j => j.errors?.length > 0 || j.tests?.length > 0
    );

    if (!hasIssues) {
        if (isPullRequest && existingComment) {
            const resolvedBody = [
                COMMENT_MARKER,
                '## :white_check_mark: CI Errors Resolved',
                '',
                'All previously reported errors have been fixed. All builds passing.',
                '',
                `*Last checked: ${new Date().toISOString().slice(0, 19)}Z*`
            ].join('\n');

            if (!existingComment.body.includes(':white_check_mark:')) {
                await github.rest.issues.updateComment({
                    owner, repo, comment_id: existingComment.id,
                    body: resolvedBody
                });
                core.info('All errors resolved — updated existing comment');
            }
        }
        core.info('No errors to report');
        return;
    }

    // ── 4. Build consolidated report ─────────────────────────────────

    // Deduplicate errors across jobs — track unique error signatures
    const globalErrors = new Map();   // signature -> { message, jobs[], file, errorCode }
    const globalWarnings = new Map();
    const globalTests = new Map();

    // Parse structured info from error lines
    function parseErrorInfo(msg) {
        const info = { file: '', line: '', errorCode: '', message: msg };

        // GCC/Clang: "path/file.cpp:123:45: error: message"
        let m = msg.match(/([^\s]+\.(cpp|h|hpp|c)):(\d+):\d+:\s*(?:fatal )?error:\s*(.*)/);
        if (m) {
            info.file = m[1];
            info.line = m[3];
            info.message = m[4];
            return info;
        }

        // MSVC: "path\file.cpp(123): error C2039: message"
        m = msg.match(/([^\s]+\.(cpp|h|hpp|c))\((\d+)\):\s*(?:fatal )?error\s+(C\d+):\s*(.*)/);
        if (m) {
            info.file = m[1].replace(/\\/g, '/');
            info.line = m[3];
            info.errorCode = m[4];
            info.message = m[5];
            return info;
        }

        // Linker: "undefined reference to `symbol'"
        m = msg.match(/undefined reference to [`']([^'`]+)/);
        if (m) {
            info.message = `undefined reference to \`${m[1]}\``;
            return info;
        }

        // MSVC linker: "unresolved external symbol ..."
        m = msg.match(/unresolved external symbol\s+(.*)/);
        if (m) {
            info.message = `unresolved external symbol: ${m[1].slice(0, 120)}`;
            return info;
        }

        return info;
    }

    function normalizeSignature(msg) {
        return msg
            .replace(/\/home\/runner\/work\/[^/]*\/[^/]*\//g, '')
            .replace(/[A-Z]:\\[^\s]*\\SparkEngine\\/g, '')
            .replace(/:\d+:\d+/g, ':N:N')
            .replace(/\(\d+\)/g, '(N)')
            .replace(/\s+/g, ' ')
            .trim();
    }

    for (const result of jobResults) {
        const job = result.job || 'unknown';

        for (const err of (result.errors || [])) {
            const sig = normalizeSignature(err);

            if (globalErrors.has(sig)) {
                const entry = globalErrors.get(sig);
                if (!entry.jobs.includes(job)) entry.jobs.push(job);
            } else {
                const info = parseErrorInfo(err);
                globalErrors.set(sig, {
                    raw: err,
                    file: info.file,
                    line: info.line,
                    errorCode: info.errorCode,
                    message: info.message,
                    jobs: [job]
                });
            }
        }

        for (const warn of (result.warnings || [])) {
            const sig = normalizeSignature(warn);

            if (!globalWarnings.has(sig)) {
                globalWarnings.set(sig, { message: warn, jobs: [job] });
            } else {
                const entry = globalWarnings.get(sig);
                if (!entry.jobs.includes(job)) entry.jobs.push(job);
            }
        }

        for (const test of (result.tests || [])) {
            const sig = test.replace(/\s+/g, ' ').trim();
            if (!globalTests.has(sig)) {
                globalTests.set(sig, { message: test, jobs: [job] });
            } else {
                const entry = globalTests.get(sig);
                if (!entry.jobs.includes(job)) entry.jobs.push(job);
            }
        }
    }

    // ── 5. Format the comment body ───────────────────────────────────
    const sections = [];

    if (globalErrors.size > 0) {
        const allJobs = [...new Set(jobResults.map(r => r.job))];

        // Group errors by file for readability
        const byFile = new Map();    // file -> entries[]
        const noFile = [];           // entries without a parsed file

        for (const [, entry] of globalErrors) {
            if (entry.file) {
                const key = entry.file;
                if (!byFile.has(key)) byFile.set(key, []);
                byFile.get(key).push(entry);
            } else {
                noFile.push(entry);
            }
        }

        const lines = ['### Build Errors'];
        lines.push('');

        // File-grouped errors as a table
        if (byFile.size > 0) {
            lines.push('| File | Line | Error | Jobs |');
            lines.push('|------|------|-------|------|');

            for (const [file, entries] of byFile) {
                for (const entry of entries) {
                    const shortFile = file.replace(/^.*?(?=SparkEngine\/|SparkEditor\/|SparkConsole\/|Tests\/|GameModules\/)/, '');
                    const code = entry.errorCode ? `\`${entry.errorCode}\` ` : '';
                    const jobList = entry.jobs.length >= allJobs.length && allJobs.length > 1
                        ? 'all'
                        : entry.jobs.map(j => j.replace('build-', '')).join(', ');
                    const msg = entry.message.length > 100
                        ? entry.message.slice(0, 97) + '...'
                        : entry.message;
                    lines.push(`| \`${shortFile}\` | ${entry.line} | ${code}${msg} | ${jobList} |`);
                }
            }
            lines.push('');
        }

        // Errors without file info (linker errors, sanitizer, etc.)
        if (noFile.length > 0) {
            lines.push('<details><summary>Other errors (' + noFile.length + ')</summary>');
            lines.push('');
            lines.push('```');
            for (const entry of noFile.slice(0, 15)) {
                const jobTag = entry.jobs.length < allJobs.length
                    ? ` [${entry.jobs.map(j => j.replace('build-', '')).join(', ')}]`
                    : '';
                lines.push(entry.message + jobTag);
            }
            lines.push('```');
            lines.push('</details>');
            lines.push('');
        }

        // Raw error output for full context (collapsible)
        const rawErrors = [...globalErrors.values()].slice(0, 20);
        if (rawErrors.some(e => e.raw.includes('\n') || e.raw !== e.message)) {
            lines.push('<details><summary>Full error output</summary>');
            lines.push('');
            lines.push('```');
            for (const entry of rawErrors) {
                lines.push(entry.raw);
            }
            lines.push('```');
            lines.push('</details>');
        }

        sections.push(lines.join('\n'));
    }

    if (globalTests.size > 0) {
        const lines = ['### Test Failures', ''];
        lines.push('| Test | Jobs |');
        lines.push('|------|------|');
        for (const [, entry] of globalTests) {
            const jobList = entry.jobs.map(j => j.replace('build-', '')).join(', ');
            const msg = entry.message.length > 120
                ? entry.message.slice(0, 117) + '...'
                : entry.message;
            lines.push(`| ${msg} | ${jobList} |`);
        }
        sections.push(lines.join('\n'));
    }

    if (globalWarnings.size > 0) {
        const allJobs = [...new Set(jobResults.map(r => r.job))];

        // Separate test warnings (flaky tests) from compiler warnings
        const testWarnings = [...globalWarnings.values()]
            .filter(w => w.message.match(/\[ *WARN *\]/));
        const compilerWarnings = [...globalWarnings.values()]
            .filter(w => !w.message.match(/\[ *WARN *\]/) &&
                (w.jobs.length < allJobs.length || allJobs.length === 1));

        if (testWarnings.length > 0) {
            const lines = ['### :warning: Test Warnings (Known Flaky)', ''];
            lines.push('These tests are registered as known flaky and do not block the build:');
            lines.push('');
            lines.push('| Test | Jobs |');
            lines.push('|------|------|');
            for (const entry of testWarnings.slice(0, 20)) {
                const jobList = entry.jobs.map(j => j.replace('build-', '')).join(', ');
                const msg = entry.message.length > 120
                    ? entry.message.slice(0, 117) + '...'
                    : entry.message;
                lines.push(`| ${msg} | ${jobList} |`);
            }
            if (testWarnings.length > 20) {
                lines.push(`| ... and ${testWarnings.length - 20} more | |`);
            }
            sections.push(lines.join('\n'));
        }

        if (compilerWarnings.length > 0) {
            const lines = [];
            lines.push(`<details><summary>Compiler Warnings (${compilerWarnings.length})</summary>`);
            lines.push('');
            lines.push('```');
            for (const entry of compilerWarnings.slice(0, 20)) {
                const jobTag = entry.jobs.length < allJobs.length
                    ? ` [${entry.jobs.map(j => j.replace('build-', '')).join(', ')}]`
                    : '';
                lines.push(entry.message + jobTag);
            }
            if (compilerWarnings.length > 20) {
                lines.push(`... and ${compilerWarnings.length - 20} more`);
            }
            lines.push('```');
            lines.push('</details>');
            sections.push(lines.join('\n'));
        }
    }

    const testWarningCount = [...globalWarnings.values()]
        .filter(w => w.message.match(/\[ *WARN *\]/)).length;
    const compilerWarningCount = globalWarnings.size - testWarningCount;

    const failedJobs = jobResults.map(r => r.job.replace('build-', '')).join(', ');
    const body = [
        COMMENT_MARKER,
        `## :x: CI Error Report`,
        '',
        `**Failed jobs:** ${failedJobs}`,
        `**Errors:** ${globalErrors.size} | **Test failures:** ${globalTests.size}` +
            (testWarningCount > 0 ? ` | **Test warnings:** ${testWarningCount}` : '') +
            ` | **Compiler warnings:** ${compilerWarningCount}`,
        '',
        ...sections,
        '',
        `*Updated: ${new Date().toISOString().slice(0, 19)}Z — this comment is updated in-place, not duplicated.*`
    ].join('\n').slice(0, 65000);

    // ── 6. Post report ────────────────────────────────────────────────
    if (isPullRequest) {
        // Deduplicate: skip if body hasn't changed
        if (existingComment) {
            const stripTimestamp = s => s.replace(/\*Updated:.*\*/, '').replace(/\*Last checked:.*\*/, '');
            if (stripTimestamp(existingComment.body) === stripTimestamp(body)) {
                core.info('Error report unchanged — skipping update');
                return;
            }

            await github.rest.issues.updateComment({
                owner, repo, comment_id: existingComment.id, body
            });
            core.info(`Updated existing error report comment #${existingComment.id}`);
        } else {
            await github.rest.issues.createComment({
                owner, repo, issue_number: prNumber, body
            });
            core.info('Created new error report comment');
        }
    } else {
        // Push event: write to job summary (visible in Actions UI)
        const summaryBody = body.replace(COMMENT_MARKER, '').trim();
        await core.summary
            .addRaw(summaryBody)
            .write();
        core.info('Wrote error report to job summary (push event, no PR)');

        // Also set the job as failed so it's visible in the Actions UI
        core.setFailed(`CI Error Report: ${globalErrors.size} errors, ${globalTests.size} test failures`);
    }
};
