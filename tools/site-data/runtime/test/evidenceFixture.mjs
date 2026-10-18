export const REPOSITORY = 'Krilliac/SparkEngine';

export function buildRun(commit, { id = 101, attempt = 1, conclusion = 'success',
                                   runNumber = id, status = 'completed' } = {})
{
    return {
        id,
        run_number: runNumber,
        run_attempt: attempt,
        name: 'Build SparkEngine',
        path: '.github/workflows/build.yml',
        head_branch: 'Working',
        head_sha: commit,
        event: 'push',
        status,
        conclusion,
        repository: { full_name: REPOSITORY },
        head_repository: { full_name: REPOSITORY },
    };
}

export function siteStatus(commit, run, contentCommit = commit)
{
    return {
        schemaVersion: 1,
        repository: REPOSITORY,
        sourceCommit: commit,
        contentCommit,
        state: run.conclusion === 'success' ? 'current' : 'blocked',
        run: {
            id: run.id,
            attempt: run.run_attempt,
            headSha: commit,
            conclusion: run.conclusion,
            url: `https://github.com/${REPOSITORY}/actions/runs/${run.id}/attempts/${run.run_attempt}`,
        },
    };
}

export function mockGitHubApi(initialHead, initialRuns)
{
    let head = initialHead;
    let runs = initialRuns;
    let failure = null;
    const fetchEvidence = async (input) =>
    {
        if (failure !== null)
        {
            return new Response(null, { status: failure });
        }
        const url = new URL(input);
        if (url.origin !== 'https://api.github.com' ||
            !url.pathname.startsWith(`/repos/${REPOSITORY}/`))
        {
            throw new Error(`unexpected evidence URL ${url}`);
        }
        if (url.pathname.endsWith('/commits/Working'))
        {
            return Response.json({ sha: head });
        }
        if (url.pathname.endsWith('/actions/workflows/build.yml/runs'))
        {
            const selected = runs.filter((run) => run.head_sha === url.searchParams.get('head_sha'));
            return Response.json({ total_count: selected.length, workflow_runs: selected });
        }
        throw new Error(`unexpected evidence URL ${url}`);
    };
    return {
        fetchEvidence,
        setHead(value) { head = value; },
        setRuns(value) { runs = value; },
        failWith(value) { failure = value; },
    };
}
