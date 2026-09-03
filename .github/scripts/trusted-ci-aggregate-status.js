'use strict';

const BOT_LOGIN = 'github-actions[bot]';
const BOT_ID = 41898282;
const REPORTER_WORKFLOWS = Object.freeze({
    'Build Matrix Verifier': Object.freeze({
        workflowId: 345629369,
        path: '.github/workflows/build-matrix-verifier.yml',
        context: 'Build Matrix Verifier / Exact Source'
    }),
    'CodeQL Trusted Reporter': Object.freeze({
        workflowId: 344195954,
        path: '.github/workflows/codeql-report.yml',
        context: 'CodeQL Trusted / Exact Source'
    })
});

const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const normalizedWorkflowPath = value => typeof value === 'string' ? value.split('@', 1)[0] : '';

function hasTrustedCreator(status) {
    return isObject(status?.creator) && status.creator.login === BOT_LOGIN && status.creator.id === BOT_ID;
}

function validateCreatedStatus(status, expected) {
    if (!isObject(status) || !isObject(expected) ||
        !Number.isInteger(status.id) || status.id < 1 ||
        status.state !== expected.state || status.context !== expected.context ||
        status.target_url !== expected.targetUrl || !hasTrustedCreator(status)) {
        throw new Error('GitHub did not create the exact authenticated commit status.');
    }
    return status;
}

function validateLatestPendingStatus({
    combined,
    listed,
    context,
    targetSha,
    ownPendingId,
    ownPendingTargetUrl,
    authorizedTargetUrls
}) {
    const expectedAggregateDescription =
        `Trusted exact-source aggregate is evaluating ${targetSha.slice(0, 12)}.`;
    const expectedReporterDescription =
        `Trusted exact-source aggregate is awaiting both reporters for ${targetSha.slice(0, 12)}.`;
    const normalizedTargetSha = typeof targetSha === 'string' && /^[0-9a-f]{40}$/i.test(targetSha)
        ? targetSha.toLowerCase() : null;
    const combinedSha = typeof combined?.sha === 'string' && /^[0-9a-f]{40}$/i.test(combined.sha)
        ? combined.sha.toLowerCase() : null;
    const combinedStatuses = combined?.statuses;
    const total = combined?.total_count;
    if (!normalizedTargetSha || combinedSha !== normalizedTargetSha ||
        !Number.isInteger(total) || total < 1 || total > 100 ||
        !Array.isArray(combinedStatuses) || combinedStatuses.length !== total ||
        !Array.isArray(listed) || listed.length < 1 || listed.length > 100 ||
        !Array.isArray(authorizedTargetUrls) || authorizedTargetUrls.some(value => typeof value !== 'string')) {
        throw new Error('The commit-status inventory is incomplete, malformed, or unbounded.');
    }

    const combinedAggregate = combinedStatuses.filter(status => status?.context === context);
    const listedAggregate = listed.filter(status => status?.context === context);
    const current = combinedAggregate[0];
    const authenticated = listedAggregate[0];
    const authorizedTargets = new Set(authorizedTargetUrls);
    const ownPendingIsExact = current?.target_url === ownPendingTargetUrl
        ? current?.id === ownPendingId : true;
    const descriptionIsExact = current?.description === expectedAggregateDescription ||
        current?.description === expectedReporterDescription;
    if (combinedAggregate.length !== 1 || listedAggregate.length < 1 ||
        !Number.isInteger(current?.id) || current.id < 1 || authenticated?.id !== current.id ||
        authenticated?.state !== current.state || authenticated?.target_url !== current.target_url ||
        authenticated?.description !== current.description || !hasTrustedCreator(authenticated) ||
        current.state !== 'pending' || !authorizedTargets.has(current.target_url) ||
        !descriptionIsExact || !ownPendingIsExact) {
        throw new Error('The latest aggregate status is stale, forged, or not pending.');
    }
    return authenticated;
}

function hasTerminalReporterSet({ combined, listed, targetSha, contexts }) {
    const normalizedTargetSha = typeof targetSha === 'string' && /^[0-9a-f]{40}$/i.test(targetSha)
        ? targetSha.toLowerCase() : null;
    const combinedSha = typeof combined?.sha === 'string' && /^[0-9a-f]{40}$/i.test(combined.sha)
        ? combined.sha.toLowerCase() : null;
    const statuses = combined?.statuses;
    const total = combined?.total_count;
    if (!normalizedTargetSha || combinedSha !== normalizedTargetSha ||
        !Number.isInteger(total) || total < 1 || total > 100 ||
        !Array.isArray(statuses) || statuses.length !== total ||
        !Array.isArray(listed) || listed.length < 1 || listed.length > 100 ||
        !Array.isArray(contexts) || contexts.length !== 2 ||
        contexts.some(value => typeof value !== 'string' || !value)) {
        throw new Error('Terminal reporter readiness inventory is malformed.');
    }
    const terminalStates = new Set(['failure', 'success']);
    for (const context of contexts) {
        const matches = statuses.filter(status => status?.context === context);
        if (matches.length > 1) {
            throw new Error(`Terminal reporter readiness duplicated ${context}.`);
        }
        if (matches.length === 0 || matches[0].state === 'pending') {
            return false;
        }
        if (!terminalStates.has(matches[0].state)) {
            throw new Error(`Terminal reporter readiness has an invalid state for ${context}.`);
        }
        const authenticated = listed.find(status => status?.id === matches[0].id);
        if (!authenticated || authenticated.context !== matches[0].context ||
            authenticated.state !== matches[0].state ||
            authenticated.target_url !== matches[0].target_url ||
            authenticated.description !== matches[0].description ||
            !hasTrustedCreator(authenticated)) {
            throw new Error(`Terminal reporter readiness could not authenticate ${context}.`);
        }
    }
    return true;
}

function isExactCompletedReporterEvent({
    combined, targetSha, repository, serverUrl, action,
    payloadRepository, workflowRun, currentRun
}) {
    const binding = REPORTER_WORKFLOWS[workflowRun?.name];
    const normalizedTargetSha = typeof targetSha === 'string' && /^[0-9a-f]{40}$/i.test(targetSha)
        ? targetSha.toLowerCase() : null;
    const terminalConclusions = new Set([
        'action_required', 'cancelled', 'failure', 'neutral', 'skipped',
        'stale', 'success', 'timed_out'
    ]);
    if (!binding || action !== 'completed' || workflowRun?.status !== 'completed' ||
        currentRun?.status !== 'completed' || workflowRun.event !== 'workflow_run' ||
        currentRun.event !== 'workflow_run' ||
        currentRun.name !== workflowRun.name ||
        !terminalConclusions.has(workflowRun.conclusion) ||
        currentRun.conclusion !== workflowRun.conclusion ||
        normalizedWorkflowPath(workflowRun.path) !== binding.path ||
        normalizedWorkflowPath(currentRun.path) !== binding.path ||
        workflowRun.workflow_id !== binding.workflowId ||
        currentRun.workflow_id !== binding.workflowId ||
        workflowRun.head_branch !== 'Working' || currentRun.head_branch !== 'Working' ||
        String(workflowRun.head_sha || '').toLowerCase() !== normalizedTargetSha ||
        String(currentRun.head_sha || '').toLowerCase() !== normalizedTargetSha ||
        payloadRepository?.id !== repository?.id || payloadRepository?.full_name !== repository?.full_name ||
        workflowRun.repository?.id !== repository?.id ||
        workflowRun.repository?.full_name !== repository?.full_name ||
        workflowRun.head_repository?.id !== repository?.id ||
        workflowRun.head_repository?.full_name !== repository?.full_name ||
        currentRun.repository?.id !== repository?.id ||
        currentRun.repository?.full_name !== repository?.full_name ||
        currentRun.head_repository?.id !== repository?.id ||
        currentRun.head_repository?.full_name !== repository?.full_name ||
        !Number.isSafeInteger(workflowRun.id) || workflowRun.id < 1 ||
        currentRun.id !== workflowRun.id ||
        !Number.isSafeInteger(workflowRun.run_attempt) || workflowRun.run_attempt < 1 ||
        currentRun.run_attempt !== workflowRun.run_attempt ||
        !Number.isSafeInteger(workflowRun.run_number) || workflowRun.run_number < 1 ||
        currentRun.run_number !== workflowRun.run_number ||
        typeof serverUrl !== 'string' || !serverUrl ||
        !Number.isSafeInteger(repository?.id) || repository.id < 1 ||
        typeof repository?.full_name !== 'string' || !repository.full_name) {
        return false;
    }
    const matches = combined?.statuses?.filter(status => status?.context === binding.context);
    if (!Array.isArray(matches) || matches.length !== 1) {
        return false;
    }
    const expectedTarget = `${serverUrl}/${repository.full_name}/actions/runs/` +
        `${workflowRun.id}/attempts/${workflowRun.run_attempt}`;
    return matches[0].target_url === expectedTarget;
}

function hasCurrentPendingLease({
    combined, listed, context, targetSha, ownPendingId, ownPendingTargetUrl
}) {
    const normalizedTargetSha = typeof targetSha === 'string' && /^[0-9a-f]{40}$/i.test(targetSha)
        ? targetSha.toLowerCase() : null;
    const combinedSha = typeof combined?.sha === 'string' && /^[0-9a-f]{40}$/i.test(combined.sha)
        ? combined.sha.toLowerCase() : null;
    const statuses = combined?.statuses;
    const total = combined?.total_count;
    if (!normalizedTargetSha || combinedSha !== normalizedTargetSha ||
        !Number.isInteger(total) || total < 1 || total > 100 ||
        !Array.isArray(statuses) || statuses.length !== total ||
        !Array.isArray(listed) || listed.length < 1 || listed.length > 100 ||
        typeof context !== 'string' || !context ||
        !Number.isInteger(ownPendingId) || ownPendingId < 1 ||
        typeof ownPendingTargetUrl !== 'string' || !ownPendingTargetUrl) {
        throw new Error('Pending aggregate lease inventory is malformed.');
    }
    const matches = statuses.filter(status => status?.context === context);
    if (matches.length !== 1 || matches[0].id !== ownPendingId) {
        return false;
    }
    const latest = matches[0];
    const expectedDescription =
        `Trusted exact-source aggregate is evaluating ${normalizedTargetSha.slice(0, 12)}.`;
    const authenticated = listed.find(status => status?.id === ownPendingId);
    if (latest.state !== 'pending' || latest.target_url !== ownPendingTargetUrl ||
        latest.description !== expectedDescription || !authenticated ||
        authenticated.context !== latest.context || authenticated.state !== latest.state ||
        authenticated.target_url !== latest.target_url ||
        authenticated.description !== latest.description ||
        authenticated.creator?.login !== BOT_LOGIN || authenticated.creator?.id !== BOT_ID) {
        throw new Error('Current pending aggregate lease is not authenticated.');
    }
    return true;
}

async function publishValidatedTerminalStatus({
    createStatus,
    request,
    state,
    context,
    targetUrl,
    successDescription,
    failureDescription,
    pendingDescription
}) {
    const terminalDescription = state === 'success'
        ? successDescription : failureDescription;
    if (typeof createStatus !== 'function' || !isObject(request) ||
        !['failure', 'success'].includes(state) ||
        typeof terminalDescription !== 'string' || !terminalDescription) {
        throw new Error('Terminal aggregate status publication inputs are invalid.');
    }
    try {
        const response = await createStatus({
            ...request,
            state,
            target_url: targetUrl,
            description: terminalDescription,
            context
        });
        return validateCreatedStatus(response?.data, { state, context, targetUrl });
    } catch (error) {
        try {
            const recovery = await createStatus({
                ...request,
                state: 'pending',
                target_url: targetUrl,
                description: pendingDescription,
                context
            });
            validateCreatedStatus(recovery?.data, {
                state: 'pending', context, targetUrl
            });
        } catch (recoveryError) {
            throw new AggregateError(
                [error, recoveryError],
                'Terminal aggregate status was uncertain and its pending override also failed.'
            );
        }
        throw error;
    }
}

module.exports = Object.freeze({
    hasCurrentPendingLease,
    hasTerminalReporterSet,
    isExactCompletedReporterEvent,
    publishValidatedTerminalStatus,
    validateCreatedStatus,
    validateLatestPendingStatus
});
