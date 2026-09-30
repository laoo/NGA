# The version the binary reports is the tag the build was made from.
#
# There is no number in this tree to keep in step with the tags, which is why
# `project()` carries no `VERSION`: a literal there would be a second answer to
# a question the tags already answer, and the two drifted apart once already --
# a tag went out on a binary that named a different release.
#
# The tags live in the public repository, the one a release is cut from. This
# tree has none, so a build here reports `devel`, and so does a build from a
# source archive with no repository around it. That is the honest answer: a
# binary that is not a release should not name one.
#
# `git describe` is asked at configure time rather than at build time, because a
# version string does not earn a target that runs whether anything changed or
# not. What keeps the answer fresh is CMAKE_CONFIGURE_DEPENDS over the four
# places git writes when the answer could have changed. Which four was measured,
# not assumed: a commit appends to `logs/HEAD` and leaves `HEAD` alone, a tag
# written or moved changes the mtime of the `refs/tags` directory, `HEAD` itself
# moves on a checkout, and `packed-refs` moves when the loose refs are packed
# away. Between them a configure is re-run whenever a commit or a tag could have
# changed what `describe` answers.

find_package( Git QUIET )

# Sets `outVariable` in the caller's scope to the version the build reports:
# `git describe`'s answer without its leading `v`, or `devel` where there is no
# tag to describe.
function( ngaResolveVersion outVariable )
  set( version "devel" )

  if( GIT_FOUND )
    # `.git` is a file and not a directory inside a worktree, so the directory
    # is asked for rather than assumed.
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE gitDir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE outsideRepository )

    if( outsideRepository EQUAL 0 )
      # `refs/tags` is a directory, and a directory is watched by its mtime --
      # which is what changes when a tag file is written into it or replaced.
      foreach( ref "HEAD" "logs/HEAD" "packed-refs" "refs/tags" )
        if( EXISTS "${gitDir}/${ref}" )
          set_property( DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS "${gitDir}/${ref}" )
        endif( )
      endforeach( )

      # `--match` so that a tag that is not a release cannot become the version,
      # and `--dirty` so that a build with uncommitted changes says so.
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --match "v[0-9]*"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE described
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE noTag )

      if( noTag EQUAL 0 )
        string( REGEX REPLACE "^v" "" version "${described}" )
      endif( )
    endif( )
  endif( )

  set( ${outVariable} "${version}" PARENT_SCOPE )
endfunction( )
