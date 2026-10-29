/* Public-domain diagnostic console transport; official Wi-Fi/iperf unchanged. */
#include "simple_console.h"
#include <stdio.h>
#include <unistd.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_stdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void prompt(void)
{
    fputs("iperf> ", stdout);
    fflush(stdout);
}

static void console_task(void *context)
{
    char line[512];
    size_t length = 0;
    bool overflow = false;
    (void)context;
    puts("Plain USB console ready. Type help and press Enter (CR or LF).");
    prompt();
    while (true) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line[length] = '\0';
            if (overflow) {
                puts("Command too long; discarded.");
            } else if (length) {
                int result = 0;
                const esp_err_t err = esp_console_run(line, &result);
                if (err == ESP_ERR_NOT_FOUND) puts("Unrecognized command; type help.");
                else if (err != ESP_OK) printf("Console error: %s\n", esp_err_to_name(err));
                else if (result != 0) printf("Command failed: %d\n", result);
            }
            length = 0;
            overflow = false;
            prompt();
        } else if (c == 3 || c == 21) { // Ctrl+C / Ctrl+U clear an unfinished command.
            length = 0;
            overflow = false;
            putchar('\n');
            prompt();
        } else if (c == '\b' || c == 127) {
            if (length && !overflow) {
                --length;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
        } else if ((unsigned char)c >= 32) {
            if (length + 1 < sizeof(line) && !overflow) {
                line[length++] = c;
                putchar((unsigned char)c);
                fflush(stdout);
            } else overflow = true;
        }
    }
}

void simple_console_init(void)
{
    // Install the blocking USB input driver before reading. No ANSI terminal probe.
    ESP_ERROR_CHECK(esp_stdio_install_io_driver());
    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_length = 512;
    ESP_ERROR_CHECK(esp_console_init(&config));
    ESP_ERROR_CHECK(esp_console_register_help_command());
}

void simple_console_start(void)
{
    // Same stack and priority as the official REPL; task blocks while idle.
    ESP_ERROR_CHECK(xTaskCreate(console_task, "plain_console", 4096, NULL, 2, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}
