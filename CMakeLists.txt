cmake_minimum_required(VERSION 3.8)
project(a3_e0x9a_o9j0b)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES
        mailuser.c
        mailuser.h
        mypopd.c
        mysmtpd.c
        netbuffer.c
        netbuffer.h
        server.c
        server.h)

add_executable(a3_e0x9a_o9j0b ${SOURCE_FILES})