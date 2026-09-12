idf_component_register(SRCS "blink_example_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES 
                           esp_wifi 
                           nvs_flash 
                           esp_netif 
                           esp_http_server 
                           driver
)