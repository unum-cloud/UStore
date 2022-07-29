# Download Networking TS for us to implement RPC
# Example can be found here: https://github.com/bsergeev/CppNetworkingTS
# Discussions: https://www.reddit.com/r/cpp/comments/b12iob/networking_ts_beast_new_tutorials_read_this_to/
find_package(Git)

if(NOT GIT_FOUND)
    message(FATAL_ERROR "Git not found, aborting...")
endif()

set(NETTS_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/netts)
find_path(NETTS_INCLUDE_DIR "net" NO_DEFAULT_PATH PATHS "${NETTS_LOCATION}/include/experimental")

if(NOT NETTS_INCLUDE_DIR) # The repo wasn't found => clone it
    execute_process(COMMAND git clone https://github.com/chriskohlhoff/networking-ts-impl.git ${NETTS_LOCATION})
else() # Pull the repo, in case it was updated
    execute_process(COMMAND git pull WORKING_DIRECTORY ${NETTS_LOCATION})
endif()

find_path(NETTS_INCLUDE_DIR "net" NO_DEFAULT_PATH PATHS "${NETTS_LOCATION}/include/experimental")

if(NOT NETTS_INCLUDE_DIR)
    message(FATAL_ERROR "Something wrong with networking-ts-impl.git, aborting...")
endif()

include_directories("${NETTS_LOCATION}/include")
