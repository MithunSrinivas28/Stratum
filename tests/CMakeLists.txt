add_executable(stratum_resp_tests
    resp_parser_test.cpp
    resp_encoder_test.cpp
)

target_link_libraries(stratum_resp_tests PRIVATE
    stratum_resp
    GTest::gtest_main
)

add_executable(stratum_engine_tests
    storage_executor_test.cpp
)

target_link_libraries(stratum_engine_tests PRIVATE
    stratum_engine
    stratum_resp
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(stratum_resp_tests)
gtest_discover_tests(stratum_engine_tests)
