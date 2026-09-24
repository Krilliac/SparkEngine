#!/usr/bin/env python3
"""Recheck channel policy in the same call path as one remote mutation."""
import os
import subprocess
import sys

from verify_release_policy import verify


def guarded_run(command, repository, is_versioned, *, runner=subprocess.run, policy_check=verify,
                input=None, capture_output=False, text=False):
    allowed = bool(command) and (
        command[0] == "git" and "push" in command[1:]
        or command[:2] == ["gh", "api"] and any(
            command[index] in {"--method", "-X"} and command[index + 1] in {"POST", "PATCH", "PUT", "DELETE"}
            for index in range(2, len(command) - 1)
        )
    )
    if not allowed:
        raise ValueError("guard accepts only a Git push or an explicit GitHub API mutation")
    # No preparation or metadata work may be placed between this proof and the
    # child call. Every caller must invoke the guard for each separate write.
    policy_check(repository, is_versioned)
    mutation_environment = dict(os.environ)
    mutation_environment.pop("RELEASE_POLICY_READ_TOKEN", None)
    return runner(command, input=input, capture_output=capture_output, text=text,
                  env=mutation_environment, check=False, timeout=900)


def main():
    command = sys.argv[1:]
    if command[:1] == ["--"]:
        command = command[1:]
    channel = os.environ.get("IS_VERSIONED")
    if channel not in {"true", "false"}:
        print("mutation guard requires an explicit release channel", file=sys.stderr)
        return 1
    try:
        result = guarded_run(command, os.environ.get("GITHUB_REPOSITORY", ""), channel == "true")
        return result.returncode
    except ValueError as error:
        print(f"release mutation refused: {error}", file=sys.stderr)
    except (OSError, subprocess.SubprocessError):
        # Command arguments can contain a Git authorization header. Never echo
        # argv, CalledProcessError, or a subprocess environment here.
        print("release mutation could not execute; inspect the command diagnostics", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
