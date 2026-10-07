add_executable(stratum_resp_tests
    resp_parser_test.cpp
)

target_link_libraries(stratum_resp_tests PRIVATE
    stratum_resp
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(stratum_resp_tests)
