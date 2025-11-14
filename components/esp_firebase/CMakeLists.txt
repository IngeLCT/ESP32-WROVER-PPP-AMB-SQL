idf_component_register(
	SRCS "app.cpp" "rtdb.cpp" "firebase_c_shim.cpp"
	INCLUDE_DIRS "." "include"
	REQUIRES jsoncpp esp_http_client esp_netif nvs_flash mbedtls esp-tls
)
# Make main's include path (for privado.h) visible to this component
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/main")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_11)
