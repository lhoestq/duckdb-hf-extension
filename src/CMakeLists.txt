set(HF_SOURCES
    hf_extension.cpp
    hf_settings.cpp
    hf_secrets.cpp
    hf_url.cpp
    hf_crypto.cpp
    hf_http.cpp
    hf_fs.cpp
    hf_read.cpp
    hf_commit.cpp
    hf_write.cpp
    hf_functions.cpp)

add_library(hf_library OBJECT ${HF_SOURCES})

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:hf_library>
    PARENT_SCOPE)
