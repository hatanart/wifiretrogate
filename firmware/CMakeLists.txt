# WifiRetroGate minimal bridge test firmware for the WT32-ETH01
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(wrg_bridge)
