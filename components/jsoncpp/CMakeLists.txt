idf_component_register( SRCS "json_reader.cpp" "json_writer.cpp" "json_value.cpp" INCLUDE_DIRS "." ) 
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_11)
# JsonCpp without C++ exceptions (ESP-IDF uses -fno-exceptions)
target_compile_definitions(${COMPONENT_LIB} PRIVATE JSON_USE_EXCEPTION=0)
