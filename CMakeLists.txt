cmake_minimum_required(VERSION 3.10)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# set the project name
project(LifeLockTest)

# add the executable
add_executable(LifeLockTest test/main.cpp include/life_lock.h)

target_include_directories(LifeLockTest PUBLIC "include")
