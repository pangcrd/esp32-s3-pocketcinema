file(GLOB_RECURSE UI_SRCS "ui/*.c")
idf_component_register(SRCS ${UI_SRCS}
    INCLUDE_DIRS "ui"
    REQUIRES lvgl)

