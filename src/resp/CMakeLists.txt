add_library(stratum_resp STATIC
    resp_parser.cpp
    resp_encoder.cpp
)

target_include_directories(stratum_resp PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
