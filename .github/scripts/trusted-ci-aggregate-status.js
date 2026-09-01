'use strict';

const BOT_LOGIN = 'github-actions[bot]';
const BOT_ID = 41898282;

const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);

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
    const combinedStatuses = combined?.statuses;
    const total = combined?.total_count;
    if (!Number.isInteger(total) || total < 1 || total > 100 ||
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

module.exports = Object.freeze({
    validateCreatedStatus,
    validateLatestPendingStatus
});
