set(COMPONENT_SRCDIRS "src" "src/cpu")
set(COMPONENT_ADD_INCLUDEDIRS "." "src" "src/cpu")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-implicit-fallthrough
    -DWS_NO_SPLASH_SCREEN
    -DWS_SAVE_ENABLED
    -O2
)
