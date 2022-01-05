include(FetchContent)

function(__get_home_dir home)
  if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(HOMEDRIVE $ENV{HOMEDRIVE})
    set(HOMEPATH $ENV{HOMEPATH})
    if (HOMEDRIVE AND HOMEPATH)
      file(TO_CMAKE_PATH $ENV{HOMEDRIVE}$ENV{HOMEPATH} HOME)
      set(${home} ${HOME} PARENT_SCOPE)
    else()
      set(${home} $ENV{HOME} PARENT_SCOPE)
    endif()
  else()
    set(${home} $ENV{HOME} PARENT_SCOPE)
  endif()
endfunction()

function(cxxpm_initialize url hash)
  # Check for installed cxx-pm
  __get_home_dir(USER_HOME_DIRECTORY)
  if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(CXXPM_EXECUTABLE ${USER_HOME_DIRECTORY}/.cxxpm/self/cxx-pm.exe)
  else()
    set(CXXPM_EXECUTABLE ${USER_HOME_DIRECTORY}/.cxxpm/self/cxx-pm)
  endif()
  
  if (EXISTS ${CXXPM_EXECUTABLE})
      execute_process(COMMAND ${CXXPM_EXECUTABLE} "--version" OUTPUT_VARIABLE INSTALLED_VERSION_OUTPUT)
      string(STRIP ${INSTALLED_VERSION_OUTPUT} INSTALLED_VERSION)
  endif()
  
  if (INSTALLED_VERSION)
    message("Found installed cxx-pm version ${INSTALLED_VERSION}")
  endif()
  
  # Download requested version
  #  string(SUBSTRING <string> <begin> <length> <out-var>)
  string(SUBSTRING ${hash} 0 8 HASH32)
  FetchContent_Declare(cxx-pm-${HASH32}
    URL ${url}
    URL_HASH SHA256=${hash}
  )
  
  FetchContent_MakeAvailable(cxx-pm-${HASH32})
  file(STRINGS ${cxx-pm-${HASH32}_SOURCE_DIR}/src/CXXPM_VERSION REQUESTED_VERSION)
  if (NOT REQUESTED_VERSION)
    message(FATAL_ERROR "cxx-pm package does not contains version information")
  endif()
  message("Downloaded cxx-pm version ${REQUESTED_VERSION}")
  
  if ((NOT INSTALLED_VERSION) OR (INSTALLED_VERSION VERSION_LESS REQUESTED_VERSION))
    # Install new cxx-pm
    message("Install cxx-pm ${REQUESTED_VERSION} ...")

    # Remove directory ${HOME}/cxx-pm/self
    file(REMOVE_RECURSE ${USER_HOME_DIRECTORY}/.cxxpm/self)

    # Configure
    execute_process(COMMAND ${CMAKE_COMMAND} "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=${USER_HOME_DIRECTORY}/.cxxpm/self" "${cxx-pm-${HASH32}_SOURCE_DIR}/src" WORKING_DIRECTORY ${cxx-pm-${HASH32}_BINARY_DIR} RESULT_VARIABLE EXIT_CODE)
    if (NOT (EXIT_CODE EQUAL 0))
      message(FATAL_ERROR "cxx-pm configure stage failed")
    endif()

    # Build & install
    execute_process(COMMAND ${CMAKE_COMMAND} "--build" "." "--target" "install" "--config" "Release" WORKING_DIRECTORY ${cxx-pm-${HASH32}_BINARY_DIR} RESULT_VARIABLE EXIT_CODE)
    if (NOT (EXIT_CODE EQUAL 0))
      message(FATAL_ERROR "cxx-pm build/install stage failed")
    endif()

    set_property(GLOBAL PROPERTY CXXPM_EXECUTABLE ${CXXPM_EXECUTABLE})
    include(${USER_HOME_DIRECTORY}/.cxxpm/self/add.cmake)
  else()
    message("installed cxx-pm version is same or newer, nothing to do")
    get_property(CXXPM GLOBAL PROPERTY CXXPM_EXECUTABLE)
    if (NOT CXXPM)
      include(${USER_HOME_DIRECTORY}/.cxxpm/self/add.cmake)
      set_property(GLOBAL PROPERTY CXXPM_EXECUTABLE ${CXXPM_EXECUTABLE})
    endif()
  endif()
endfunction()
