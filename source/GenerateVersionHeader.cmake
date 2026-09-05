# Run via `cmake -P` as a custom target's command (see CMakeLists.txt),
# not as part of the normal configure step - execute_process()'d directly
# in CMakeLists.txt only re-runs when `cmake ..` itself re-runs, so the
# embedded git hash would go stale after a plain `cmake --build .` with no
# fresh reconfigure. Running as an ALWAYS-built custom target instead means
# this script re-checks the git state on every single build.
#
# Expects -DSRC_DIR=<dir inside the git repo> and -DOUT_FILE=<path to write>
# on the command line (see CMakeLists.txt's add_custom_target()).
#
# --always falls back to a raw commit hash if there's no tag to describe
# from; --dirty appends "-dirty" the moment the working tree has uncommitted
# changes on top of that commit - without it, two builds from the same
# commit but different uncommitted edits would silently claim the same
# version.
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${SRC_DIR}
    OUTPUT_VARIABLE OLLI_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)

if(NOT GIT_RESULT EQUAL 0 OR NOT OLLI_GIT_VERSION)
    set(OLLI_GIT_VERSION "unknown")
endif()

# configure_file() already skips touching OUT_FILE's mtime if the
# substituted content would be identical to what's already there - exactly
# the "don't force a rebuild of everything that includes this header unless
# the version actually changed" behavior we want, so no manual read/compare
# needed here.
configure_file(${SRC_DIR}/version.h.in ${OUT_FILE} @ONLY)
