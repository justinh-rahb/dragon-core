idf_component_register(
    SRCS "dc_ui.c"
    INCLUDE_DIRS "include"
)

# Keep the editable, dependency-free SPA as HTML and create a reproducible gzip
# blob at build time. Products serve it through their existing HTTP stack.
find_program(GZIP_EXECUTABLE gzip REQUIRED)
set(DC_UI_HTML "${COMPONENT_DIR}/www/app.html")
set(DC_UI_HTML_GZ "${CMAKE_CURRENT_BINARY_DIR}/dc_ui_app.html.gz")
add_custom_command(
    OUTPUT "${DC_UI_HTML_GZ}"
    COMMAND "${GZIP_EXECUTABLE}" -9 -n -c "${DC_UI_HTML}" > "${DC_UI_HTML_GZ}"
    MAIN_DEPENDENCY "${DC_UI_HTML}"
    COMMENT "Gzipping Dragon-family SPA"
    VERBATIM
)
target_add_binary_data(${COMPONENT_LIB} "${DC_UI_HTML_GZ}" BINARY)
