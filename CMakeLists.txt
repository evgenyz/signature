cmake_minimum_required(VERSION 3.0)
project(signature)

#set(Boost_USE_STATIC_LIBS on)
add_definitions(-DBOOST_LOG_DYN_LINK)
find_package(Boost 1.60 COMPONENTS system log thread program_options REQUIRED)

include_directories(${Boost_INCLUDE_DIRS})

set(SRC main.cpp hasher.cpp)

add_executable(${PROJECT_NAME} ${SRC})
target_link_libraries(${PROJECT_NAME} ${Boost_LOG_LIBRARY} ${Boost_PROGRAM_OPTIONS_LIBRARY} Boost::thread)

install(TARGETS ${PROJECT_NAME} DESTINATION bin)