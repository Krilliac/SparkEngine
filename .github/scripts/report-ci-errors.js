// report-ci-errors.js — Consolidated, deduplicated CI error reporter
//
// Called by the report-ci-errors job after all build jobs complete.
// Downloads error summary artifacts from each failed job, consolidates
// them into one PR comment, and deduplicates against existing comments
// to prevent spam.
//
// Strategy:
//   1. Find or create a single "CI Error Report" comment (identified by marker)
//   2. Merge all job error summaries into one report
//   3. Compare against the previous comment body — skip update if identical
//   4. On success (no errors), delete the old error comment if it exists

const COMMENT_MARKER = '<!-- spark-ci-error-report -->';

module.exports = async ({ github, context, core, glob, io, artifact }) => {
    const owner = context.repo.owner;
    const repo = context.repo.repo;
    const prNumber = context.issue.number;

    if (!prNumber) {
        core.info('Not a pull request — skipping error report');
        return;
    }

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

    // ── 2. Find existing error report comment ────────────────────────
    let existingComment = null;
    const comments = await github.rest.issues.listComments({
        owner, repo, issue_number: prNumber, per_page: 100
    });

    for (const comment of comments.data) {
        if (comment.body && comment.body.includes(COMMENT_MARKER)) {
            existingComment = comment;
            break;
        }
    }

    // ── 3. If no errors found, clean up old comment ──────────────────
    const hasIssues = jobResults.some(
        j => j.errors?.length > 0 || j.tests?.length > 0
    );

    if (!hasIssues) {
        if (existingComment) {
            // All green — update existing comment to show resolved
            const resolvedBody = [
                COMMENT_MARKER,
                '## :white_check_mark: CI Errors Resolved',
                '',
                'All previously reported errors have been fixed. All builds passing.',
                '',
                `*Last checked: ${new Date().toISOString().slice(0, 19)}Z*`
            ].join('\n');

            // Only update if it's not already showing resolved
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
    const globalErrors = new Map();   // signature -> { message, jobs[] }
    const globalWarnings = new Map();
    const globalTests = new Map();

    for (const result of jobResults) {
        const job = result.job || 'unknown';

        for (const err of (result.errors || [])) {
            // Normalize: strip paths, line numbers for dedup signature
            const sig = err
                .replace(/\/home\/[^\s:]*/g, '')
                .replace(/[A-Z]:\\[^\s:]*/g, '')
                .replace(/:\d+:\d+/g, ':N:N')
                .replace(/\s+/g, ' ')
                .trim();

            if (globalErrors.has(sig)) {
                const entry = globalErrors.get(sig);
                if (!entry.jobs.includes(job)) entry.jobs.push(job);
            } else {
                globalErrors.set(sig, { message: err, jobs: [job] });
            }
        }

        for (const warn of (result.warnings || [])) {
            const sig = warn
                .replace(/\/home\/[^\s:]*/g, '')
                .replace(/[A-Z]:\\[^\s:]*/g, '')
                .replace(/:\d+:\d+/g, ':N:N')
                .replace(/\s+/g, ' ')
                .trim();

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
        const lines = [];
        // Group: errors seen in all compilers vs compiler-specific
        const allJobs = [...new Set(jobResults.map(r => r.job))];
        const universal = [];
        const specific = [];

        for (const [, entry] of globalErrors) {
            if (entry.jobs.length >= allJobs.length && allJobs.length > 1) {
                universal.push(entry.message);
            } else {
                specific.push({ ...entry });
            }
        }

        if (universal.length > 0) {
            lines.push('### Build Errors (all compilers)');
            lines.push('```');
            lines.push(...universal.slice(0, 20));
            lines.push('```');
        }

        if (specific.length > 0) {
            // Group by job
            const byJob = {};
            for (const entry of specific) {
                for (const job of entry.jobs) {
                    if (!byJob[job]) byJob[job] = [];
                    byJob[job].push(entry.message);
                }
            }
            for (const [job, msgs] of Object.entries(byJob)) {
                lines.push(`### Build Errors — ${job}`);
                lines.push('```');
                lines.push(...[...new Set(msgs)].slice(0, 15));
                lines.push('```');
            }
        }

        sections.push(lines.join('\n'));
    }

    if (globalTests.size > 0) {
        const lines = ['### Test Failures', '```'];
        for (const [, entry] of globalTests) {
            const jobTag = entry.jobs.length < jobResults.length
                ? ` [${entry.jobs.join(', ')}]`
                : '';
            lines.push(entry.message + jobTag);
        }
        lines.push('```');
        sections.push(lines.join('\n'));
    }

    if (globalWarnings.size > 0) {
        // Only show compiler-specific warnings (ones not shared by all compilers)
        const allJobs = [...new Set(jobResults.map(r => r.job))];
        const specificWarnings = [...globalWarnings.values()]
            .filter(w => w.jobs.length < allJobs.length || allJobs.length === 1);

        if (specificWarnings.length > 0) {
            const lines = [];
            const byJob = {};
            for (const entry of specificWarnings) {
                for (const job of entry.jobs) {
                    if (!byJob[job]) byJob[job] = [];
                    byJob[job].push(entry.message);
                }
            }
            for (const [job, msgs] of Object.entries(byJob)) {
                lines.push(`<details><summary>Warnings — ${job} (${msgs.length})</summary>\n`);
                lines.push('```');
                lines.push(...[...new Set(msgs)].slice(0, 10));
                lines.push('```');
                lines.push('</details>\n');
            }
            sections.push(lines.join('\n'));
        }
    }

    const failedJobs = jobResults.map(r => r.job).join(', ');
    const body = [
        COMMENT_MARKER,
        `## :x: CI Error Report`,
        '',
        `**Failed jobs:** ${failedJobs}`,
        `**Unique errors:** ${globalErrors.size} | **Test failures:** ${globalTests.size} | **Warnings:** ${globalWarnings.size}`,
        '',
        ...sections,
        '',
        `*Updated: ${new Date().toISOString().slice(0, 19)}Z — this comment is updated in-place, not duplicated.*`
    ].join('\n').slice(0, 65000);

    // ── 6. Deduplicate: skip if body hasn't changed ──────────────────
    if (existingComment) {
        // Compare error content (ignore timestamp)
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
};
