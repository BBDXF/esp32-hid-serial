idf_component_register(
    SRCS "app_main.c" "config.c" "uart_proto.c" "rpc.c" "wifi_mgr.c"
         "http_srv.c" "hid_dev.c" "hid_actions.c" "hid_ble.c" "hid_report.c" "keymap.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
             esp_timer json bt esp_hid driver esp_app_format
    EMBED_TXTFILES "web/index.html")
