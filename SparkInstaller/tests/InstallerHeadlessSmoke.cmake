# CI-110: run the shipped SparkInstaller executable through its headless install
# path against a local fixture repository, with no network. Proves argument
# parsing, the clone-into-staging-then-activate transaction, the pending-install
# marker, the resume path on a second run, and the refusal to install into a
# non-empty directory -- the process-level behaviour the in-process
# SparkInstallerTransactionTests cannot show for the real binary.
#
# Inputs: SPARK_INSTALLER (the SparkInstaller executable), SPARK_GIT (git) and
# SPARK_WORK_DIR (a scratch directory this test owns and recreates).

foreach(_required IN ITEMS SPARK_INSTALLER SPARK_GIT SPARK_WORK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()
if(NOT EXISTS "${SPARK_INSTALLER}" OR IS_DIRECTORY "${SPARK_INSTALLER}")
    message(FATAL_ERROR "SparkInstaller binary is missing: ${SPARK_INSTALLER}")
endif()

set(_work "${SPARK_WORK_DIR}")
cmake_path(ABSOLUTE_PATH _work NORMALIZE)
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

function(_spark_git)
    execute_process(
        COMMAND "${SPARK_GIT}" -c user.name=SparkInstallerSmoke -c user.email=smoke@invalid
            -c init.defaultBranch=fixture -c core.autocrlf=false ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed (${_result}): ${_error}")
    endif()
    string(STRIP "${_output}" _output)
    set(_spark_git_output "${_output}" PARENT_SCOPE)
endfunction()

# Fixture: one commit on branch 'fixture', published as a bare repository.
set(_seed "${_work}/seed")
set(_repo "${_work}/fixture.git")
set(_content "SparkInstaller headless smoke fixture\n")
file(MAKE_DIRECTORY "${_seed}")
file(WRITE "${_seed}/README.md" "${_content}")
_spark_git(init -q "${_seed}")
_spark_git(-C "${_seed}" add README.md)
_spark_git(-C "${_seed}" commit -q -m "fixture")
_spark_git(-C "${_seed}" rev-parse HEAD)
set(_commit "${_spark_git_output}")
_spark_git(clone -q --bare "${_seed}" "${_repo}")

set(_dest "${_work}/install")
set(_install_args --headless --repo "${_repo}" --dest "${_dest}" --ref fixture --skip-build --skip-submodules)

function(_spark_run_installer expected_exit)
    execute_process(
        COMMAND "${SPARK_INSTALLER}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT 45)
    if(NOT "${_result}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR
            "SparkInstaller ${ARGN}\nexited '${_result}', expected ${expected_exit}\n"
            "stdout:\n${_output}\nstderr:\n${_error}")
    endif()
    set(_spark_installer_output "${_output}" PARENT_SCOPE)
endfunction()

# 1. A fresh headless install clones the requested ref and activates it.
_spark_run_installer(0 ${_install_args})
if(NOT _spark_installer_output MATCHES "Mode: Install")
    message(FATAL_ERROR "first run did not take the Install path:\n${_spark_installer_output}")
endif()
if(NOT IS_DIRECTORY "${_dest}/.git")
    message(FATAL_ERROR "install destination is not a git checkout: ${_dest}")
endif()
file(READ "${_dest}/README.md" _installed_content)
if(NOT _installed_content STREQUAL _content)
    message(FATAL_ERROR "installed README.md does not match the fixture commit")
endif()
_spark_git(-C "${_dest}" rev-parse HEAD)
if(NOT _spark_git_output STREQUAL _commit)
    message(FATAL_ERROR "installed HEAD ${_spark_git_output} is not the fixture commit ${_commit}")
endif()
# --skip-build stops before configure, so the install stays pending on the cloned commit.
set(_marker "${_dest}/.sparkengine-install.pending")
if(NOT EXISTS "${_marker}")
    message(FATAL_ERROR "pending-install marker was not written: ${_marker}")
endif()
file(READ "${_marker}" _marker_content)
if(NOT _marker_content STREQUAL "ref=fixture\ncommit=${_commit}\n")
    message(FATAL_ERROR "pending-install marker does not record the cloned ref and commit:\n${_marker_content}")
endif()
file(GLOB _staging LIST_DIRECTORIES true "${_work}/.install.sparkinstall-*")
if(_staging)
    message(FATAL_ERROR "the staging clone was left behind: ${_staging}")
endif()

# 2. A second run finds the pending marker and resumes the same commit.
_spark_run_installer(0 ${_install_args})
if(NOT _spark_installer_output MATCHES "Mode: Resume install")
    message(FATAL_ERROR "second run did not resume the pending install:\n${_spark_installer_output}")
endif()

# 3. A non-empty destination that is not an engine clone is refused untouched (exit 4).
set(_occupied "${_work}/occupied")
file(WRITE "${_occupied}/keep.txt" "user data\n")
_spark_run_installer(4 --headless --repo "${_repo}" --dest "${_occupied}" --ref fixture --skip-build --skip-submodules)
file(READ "${_occupied}/keep.txt" _kept)
if(NOT _kept STREQUAL "user data\n" OR EXISTS "${_occupied}/.git")
    message(FATAL_ERROR "the refused install changed the occupied destination")
endif()

message(STATUS "SparkInstaller headless install, resume and refusal verified at ${_commit}")
