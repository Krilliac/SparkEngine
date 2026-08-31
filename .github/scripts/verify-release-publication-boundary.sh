#!/usr/bin/env bash
set -euo pipefail

phase="${1:-}"
case "$phase" in
  publish-preflight)
    expected_draft=true
    ;;
  post-publish-preflight)
    expected_draft=false
    ;;
  *)
    echo "Usage: $0 {publish-preflight|post-publish-preflight}" >&2
    exit 2
    ;;
esac

for name in \
  EXPECTED_STAGED_COMMIT GH_TOKEN GITHUB_REPOSITORY GITHUB_RUN_ATTEMPT \
  GITHUB_RUN_ID GITHUB_SHA GITHUB_WORKSPACE IS_VERSIONED RELEASE_ID \
  RELEASE_TAG RUNNER_TEMP STATE_REF TARGET_SHA; do
  if [[ -z "${!name:-}" ]]; then
    echo "Missing required publication-boundary environment value: $name" >&2
    exit 2
  fi
done

if [[ ! "$RELEASE_ID" =~ ^[1-9][0-9]*$ ]] || \
   [[ ! "$TARGET_SHA" =~ ^[0-9a-f]{40}$ ]] || \
   [[ ! "$EXPECTED_STAGED_COMMIT" =~ ^[0-9a-f]{40}$ ]] || \
   [[ "$GITHUB_SHA" != "$TARGET_SHA" ]] || \
   [[ "$STATE_REF" != "refs/tags/generated-release-counters" ]]; then
  echo "Publication-boundary identity is malformed." >&2
  exit 2
fi

read_output() {
  local key="$1"
  local -a matches=()
  mapfile -t matches < <(sed -n "s/^${key}=//p" "$boundary_output")
  if [[ "${#matches[@]}" -ne 1 || -z "${matches[0]}" ]]; then
    echo "Publication-boundary verifier emitted an invalid ${key} output." >&2
    return 1
  fi
  printf '%s' "${matches[0]}"
}

auth_header="AUTHORIZATION: basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64 -w0)"
target_commit="$(git rev-parse "${TARGET_SHA}^{commit}")"
repository_url="https://github.com/${GITHUB_REPOSITORY}.git"
git -c "http.https://github.com/.extraheader=${auth_header}" \
  fetch --force --no-tags "$repository_url" \
  "+refs/tags/${RELEASE_TAG}:refs/tags/${RELEASE_TAG}"
remote_release_commit="$(git rev-parse "refs/tags/${RELEASE_TAG}^{commit}")"
if [[ "$remote_release_commit" != "$target_commit" ]]; then
  echo "Release tag ${RELEASE_TAG} drifted to ${remote_release_commit}; expected ${target_commit}." >&2
  exit 1
fi

local_staged_commit="$(git -C "$GITHUB_WORKSPACE/badge-repository" rev-parse HEAD)"
mapfile -t state_rows < <(
  git -C "$GITHUB_WORKSPACE/badge-repository" \
    -c "http.https://github.com/.extraheader=${auth_header}" \
    ls-remote --refs "$repository_url" "$STATE_REF"
)
if [[ "${#state_rows[@]}" -ne 1 ]]; then
  echo "Generated release-counter state ref is missing or ambiguous." >&2
  exit 1
fi
read -r remote_staged_commit remote_state_ref extra <<<"${state_rows[0]}"
if [[ -n "${extra:-}" || "$remote_state_ref" != "$STATE_REF" || \
      "$local_staged_commit" != "$EXPECTED_STAGED_COMMIT" || \
      "$remote_staged_commit" != "$EXPECTED_STAGED_COMMIT" ]]; then
  echo "Generated release-counter state drifted at the publication boundary." >&2
  exit 1
fi

current_working_sha="$(gh api "repos/$GITHUB_REPOSITORY/commits/Working" --jq '.sha')"
if [[ "$current_working_sha" != "$TARGET_SHA" ]]; then
  echo "Working advanced to ${current_working_sha}; expected ${TARGET_SHA}." >&2
  exit 1
fi

boundary_output="$(mktemp "$RUNNER_TEMP/release-boundary-ledger.XXXXXX")"
release_output="$(mktemp "$RUNNER_TEMP/release-boundary-release.XXXXXX")"
assets_output="$(mktemp "$RUNNER_TEMP/release-boundary-assets.XXXXXX")"
policy_output="$(mktemp "$RUNNER_TEMP/release-boundary-policy.XXXXXX")"
cleanup() {
  rm -f -- "$boundary_output" "$release_output" "$assets_output" "$policy_output"
}
trap cleanup EXIT
GITHUB_OUTPUT="$boundary_output" \
  python3 "$GITHUB_WORKSPACE/.github/scripts/prepare-download-badges.py" "$phase" \
    --expected-assets-file "$GITHUB_WORKSPACE/expected-release-assets.txt" \
    --expected-digests-file "$GITHUB_WORKSPACE/expected-release-digests.txt" \
    --data-file "$GITHUB_WORKSPACE/badge-repository/.github/badges/downloads-data.json"

boundary_exists="$(read_output target_exists)"
boundary_asset_count="$(read_output target_asset_count)"
boundary_release_id="$(read_output target_release_id)"
boundary_draft="$(read_output target_is_draft)"
if [[ "$boundary_exists" != "true" || \
      ! "$boundary_asset_count" =~ ^[1-9][0-9]*$ || \
      "$boundary_release_id" != "$RELEASE_ID" || \
      "$boundary_draft" != "$expected_draft" ]]; then
  echo "Publication-boundary release identity or visibility is not exact." >&2
  exit 1
fi

if [[ "$phase" == "publish-preflight" ]]; then
  gh api -H "X-GitHub-Api-Version: 2026-03-10" \
    "repos/$GITHUB_REPOSITORY/immutable-releases" > "$policy_output"
  if ! jq -e \
      'type == "object" and .enabled == false and .enforced_by_owner == false' \
      "$policy_output" >/dev/null; then
    echo "Release immutability is enabled or could not be proven disabled; automatic redraft recovery is unavailable." >&2
    exit 1
  fi
fi

gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" > "$release_output"
gh api --paginate --slurp \
  "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" \
  > "$assets_output"
python3 -I "$GITHUB_WORKSPACE/.github/scripts/verify_release_asset_boundary.py" \
  --release-json "$release_output" \
  --assets-json "$assets_output" \
  --ledger-json "$GITHUB_WORKSPACE/badge-repository/.github/badges/downloads-data.json" \
  --expected-assets-file "$GITHUB_WORKSPACE/expected-release-assets.txt" \
  --expected-digests-file "$GITHUB_WORKSPACE/expected-release-digests.txt" \
  --asset-directory "$GITHUB_WORKSPACE" \
  --release-id "$RELEASE_ID" \
  --release-tag "$RELEASE_TAG" \
  --is-versioned "$IS_VERSIONED" \
  --expected-draft "$expected_draft"
