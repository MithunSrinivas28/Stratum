add_library(stratum_engine STATIC
    hash_table.cpp
    executor.cpp
)

target_include_directories(stratum_engine PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(stratum_engine PUBLIC
    stratum_resp
)
