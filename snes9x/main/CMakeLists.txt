set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
rg_setup_compile_options(-O3 -fomit-frame-pointer -fjump-tables -ftree-switch-conversion -Wno-error=format)
