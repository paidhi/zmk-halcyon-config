# Personal Elora lighting module. Sources join ZMK's app target to use its
# include paths. Nothing is built unless an ELORA_LIGHTING option is set.
zephyr_include_directories(include)

target_sources_ifdef(CONFIG_ELORA_LIGHTING_MAP_DIAG app PRIVATE src/map_diag.c)
target_sources_ifdef(CONFIG_ELORA_LIGHTING app PRIVATE src/lighting.c)

if(CONFIG_ELORA_LIGHTING AND (CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR NOT CONFIG_ZMK_SPLIT))
  target_sources(app PRIVATE src/lighting_central.c)
endif()
