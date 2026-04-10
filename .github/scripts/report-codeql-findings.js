// report-codeql-findings.js — Post CodeQL SARIF findings as a PR comment
//
// Mirrors the pattern in report-ci-errors.js but for CodeQL output. Reads
// the SARIF directory produced by github/codeql-action/analyze@v4, walks
// every result, deduplicates across languages, and posts a single
// "CodeQL Report" comment on the PR (updated in-place across runs). On
// success with no findings, the existing comment is updated to a
// :white_check_mark: resolved state.
//
// Invoked from .github/workflows/codeql.yml after the analyze step.
// Expects the env var SARIF_DIR to point at the directory that contains
// the per-language .sarif files (the `output:` parameter of the
// codeql-action/analyze step). The language being analyzed should be
// passed via the env var CODEQL_LANGUAGE for scoping the comment.

const COMMENT_MARKER = '<!-- spark-codeql-report -->';

module.exports = async ({ github, context, core }) => {
    const fs = require('fs');
    const path = require('path');

    const owner = context.repo.owner;
    const repo = context.repo.repo;
    const prNumber = context.issue.number;
    const isPullRequest = !!prNumber;

    const sarifDir = process.env.SARIF_DIR || 'sarif-results';
    const language = process.env.CODEQL_LANGUAGE || 'unknown';

    // ── 1. Collect all .sarif files in the output directory ──────────
    const findings = []; // { rule, level, file, line, message, language }
    let totalResults = 0;

    if (fs.existsSync(sarifDir))
    {
        const entries = fs.readdirSync(sarifDir, { recursive: true });
        for (const entry of entries)
        {
            const fullPath = path.join(sarifDir, entry);
            if (!fs.statSync(fullPath).isFile()) continue;
            if (!entry.endsWith('.sarif')) continue;

            try
            {
                const sarif = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
                const runs = sarif.runs || [];
                for (const run of runs)
                {
                    const tool = run.tool?.driver?.name || 'CodeQL';
                    const rules = run.tool?.driver?.rules || [];
                    const ruleIndex = new Map();
                    rules.forEach((r, i) => ruleIndex.set(r.id || String(i), r));

                    for (const result of run.results || [])
                    {
                        ++totalResults;
                        const ruleId = result.ruleId || result.rule?.id || 'unknown';
                        const rule = ruleIndex.get(ruleId) || {};
                        const level = result.level || rule.defaultConfiguration?.level || 'warning';
                        const message = result.message?.text || rule.shortDescription?.text || '';
                        const loc = result.locations?.[0]?.physicalLocation;
                        const file = loc?.artifactLocation?.uri || '';
                        const line = loc?.region?.startLine || 0;

                        findings.push({
                            tool,
                            rule: ruleId,
                            ruleName: rule.name || rule.shortDescription?.text || ruleId,
                            level,
                            file,
                            line,
                            message: message.slice(0, 400),
                            language
                        });
                    }
                }
            }
            catch (e)
            {
                core.warning(`Failed to parse SARIF ${fullPath}: ${e.message}`);
            }
        }
    }
    else
    {
        core.info(`SARIF directory ${sarifDir} not found — skipping report`);
    }

    // ── 2. Find existing CodeQL report comment (PRs only) ────────────
    let existingComment = null;
    if (isPullRequest)
    {
        const comments = await github.rest.issues.listComments({
            owner, repo, issue_number: prNumber, per_page: 100
        });
        for (const comment of comments.data)
        {
            if (comment.body && comment.body.includes(COMMENT_MARKER))
            {
                existingComment = comment;
                break;
            }
        }
    }

    // ── 3. Deduplicate by (rule, file, line, message) ────────────────
    const unique = new Map();
    for (const f of findings)
    {
        const key = `${f.rule}|${f.file}|${f.line}|${f.message}`;
        if (!unique.has(key))
        {
            unique.set(key, { ...f, languages: new Set([f.language]) });
        }
        else
        {
            unique.get(key).languages.add(f.language);
        }
    }

    // ── 4. No findings → clean up old comment ────────────────────────
    if (unique.size === 0)
    {
        if (isPullRequest && existingComment)
        {
            const resolvedBody = [
                COMMENT_MARKER,
                '## :white_check_mark: CodeQL Report',
                '',
                `All previously reported CodeQL findings have been fixed. Analysis for \`${language}\` is clean.`,
                '',
                `*Last checked: ${new Date().toISOString().slice(0, 19)}Z*`
            ].join('\n');

            if (!existingComment.body.includes(':white_check_mark:'))
            {
                await github.rest.issues.updateComment({
                    owner, repo, comment_id: existingComment.id, body: resolvedBody
                });
                core.info('CodeQL clean — updated existing comment');
            }
        }
        core.info(`No CodeQL findings to report (${totalResults} results across all SARIF files)`);
        return;
    }

    // ── 5. Group by severity level ───────────────────────────────────
    const bySeverity = { error: [], warning: [], note: [], none: [] };
    for (const f of unique.values())
    {
        const lvl = bySeverity[f.level] ? f.level : 'warning';
        bySeverity[lvl].push(f);
    }

    const errorCount = bySeverity.error.length;
    const warningCount = bySeverity.warning.length;
    const noteCount = bySeverity.note.length + bySeverity.none.length;

    // ── 6. Build the comment body ────────────────────────────────────
    const lines = [];
    lines.push(COMMENT_MARKER);
    lines.push(`## :mag: CodeQL Report`);
    lines.push('');
    lines.push(`**Language:** \`${language}\``);
    lines.push(`**Errors:** ${errorCount} | **Warnings:** ${warningCount} | **Notes:** ${noteCount}`);
    lines.push('');

    const emit = (label, icon, list) =>
    {
        if (list.length === 0) return;
        lines.push(`<details${label === 'Errors' ? ' open' : ''}><summary>${icon} ${label} (${list.length})</summary>`);
        lines.push('');
        lines.push('| Rule | Location | Message |');
        lines.push('|------|----------|---------|');
        const shown = list.slice(0, 30);
        for (const f of shown)
        {
            const loc = f.file ? `\`${f.file}:${f.line}\`` : '-';
            const msg = f.message.replace(/\|/g, '\\|').replace(/\n/g, ' ');
            lines.push(`| \`${f.rule}\` | ${loc} | ${msg} |`);
        }
        if (list.length > shown.length)
        {
            lines.push(`| ... | ... | *${list.length - shown.length} more findings omitted* |`);
        }
        lines.push('');
        lines.push('</details>');
        lines.push('');
    };

    emit('Errors', ':red_circle:', bySeverity.error);
    emit('Warnings', ':warning:', bySeverity.warning);
    emit('Notes', ':information_source:', [...bySeverity.note, ...bySeverity.none]);

    lines.push('');
    lines.push(`*Updated: ${new Date().toISOString().slice(0, 19)}Z — this comment is updated in-place, not duplicated.*`);

    const body = lines.join('\n').slice(0, 65000);

    // ── 7. Post or update the PR comment ─────────────────────────────
    if (isPullRequest)
    {
        if (existingComment)
        {
            await github.rest.issues.updateComment({
                owner, repo, comment_id: existingComment.id, body
            });
            core.info(`Updated existing CodeQL comment with ${unique.size} findings`);
        }
        else
        {
            await github.rest.issues.createComment({
                owner, repo, issue_number: prNumber, body
            });
            core.info(`Posted new CodeQL comment with ${unique.size} findings`);
        }
    }
    else
    {
        // Non-PR run: write a job summary visible in the Actions UI.
        await core.summary.addRaw(body).write();
        core.info(`Wrote CodeQL job summary with ${unique.size} findings`);
    }

    // ── 8. Also emit a JSON summary for cross-workflow consumption ───
    try
    {
        const jsonSummary = {
            job: `codeql-${language}`,
            errors: bySeverity.error.map(f => `${f.file}:${f.line} [${f.rule}] ${f.message}`),
            warnings: bySeverity.warning.map(f => `${f.file}:${f.line} [${f.rule}] ${f.message}`),
            tests: []
        };
        fs.writeFileSync('codeql-summary.json', JSON.stringify(jsonSummary, null, 2));
        core.info('Wrote codeql-summary.json');
    }
    catch (e)
    {
        core.warning(`Failed to write JSON summary: ${e.message}`);
    }
};
