cmake_minimum_required(VERSION 3.10)
project(ntp-client C)

set(CMAKE_C_STANDARD 99)

add_executable(ntpclient ntpclient.c)