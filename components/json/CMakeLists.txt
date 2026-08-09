# ESP-IDF 6 moved cJSON out of the framework and the managed package now
# registers as `cjson`. Older managed components still require the historical
# IDF component name `json`, so keep this zero-source compatibility alias until
# those upstream manifests are updated.
idf_component_register(REQUIRES cjson)
