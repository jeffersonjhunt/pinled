FILE(GLOB_RECURSE app_sources src/*.cpp)

idf_component_register(
        SRCS ${app_sources}
        INCLUDE_DIRS inc
)