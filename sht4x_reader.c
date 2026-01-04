// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "include/util_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_FILE_DEFAULT "sht4x_reader.cfg"

#define MQTT_CLIENT_DEFAULT "sht4x_reader"
#define MQTT_SERVER_DEFAULT "mqtt://localhost"
#define MQTT_TOPIC_DEFAULT "server/conditions"

#define DEVICE_PATH_DEFAULT "/dev/sht4x"
#define REPORT_PERIOD_DEFAULT 60

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_MAX_ENTRIES 128

#include "include/config_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define MQTT_CONNECT_TIMEOUT 60
#define MQTT_PUBLISH_QOS 0
#define MQTT_PUBLISH_RETAIN true

#include "include/mqtt_linux.h"

MqttConfig mqttConfig;
const char *mqtt_topic;

bool mqtt_config(void) {
    mqttConfig.server = config_get_string("mqtt-server", MQTT_SERVER_DEFAULT);
    mqttConfig.client = config_get_string("mqtt-client", MQTT_CLIENT_DEFAULT);
    mqttConfig.debug = config_get_bool("debug", false);
    mqtt_topic = config_get_string("mqtt-topic", MQTT_TOPIC_DEFAULT);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    float temperature;
    float humidity;
    unsigned long serial;
    time_t timestamp;
} SensorData;

SensorData sensor_data = {0};
FILE *device_fp = NULL;
const char *device_path;
time_t report_period;
time_t report_last = 0;
bool debug_mode = false;

unsigned long messages_sent = 0;
unsigned long read_errors = 0;
time_t start_time = 0;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool sensor_read_line(void) {
    char line[256];
    int touch;
    
    if (device_fp == NULL)
        return false;
    
    while (fgets(line, sizeof(line), device_fp)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%lu, %f, %f, %d", &sensor_data.serial, &sensor_data.temperature, &sensor_data.humidity, &touch) == 4) {
            sensor_data.timestamp = time(NULL);
            return true;
        }
    }
    
    return false;
}

bool sensor_send_mqtt(void) {
    char json_buffer[512];
    char timestamp_str[32];
    
    struct tm *tm_info = gmtime(&sensor_data.timestamp);
    strftime(timestamp_str, sizeof(timestamp_str), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"temperature\":%.2f,\"humidity\":%.2f,\"timestamp\":\"%s\"}",
             sensor_data.temperature,
             sensor_data.humidity,
             timestamp_str);
    
    if (debug_mode)
        printf("sensor: sending MQTT message: %s\n", json_buffer);
    
    mqtt_send(mqtt_topic, json_buffer, (int) strlen(json_buffer));
    messages_sent++;
    
    return true;
}

bool sensor_process(void) {

    if (!sensor_read_line()) {
        read_errors++;
        fprintf(stderr, "sensor: failed to read from device (errors=%lu)\n", read_errors);
        return false;
    }
    
    if (intervalable(report_period, &report_last)) {
        printf("SHT4x: serial=%lu, temperature=%.2fC, humidity=%.2f%%, timestamp=%ld\n",
               sensor_data.serial,
               sensor_data.temperature,
               sensor_data.humidity,
               sensor_data.timestamp);
        
        if (!sensor_send_mqtt()) {
            fprintf(stderr, "sensor: failed to send MQTT message\n");
            return false;
        }
        
        printf("sensor: MQTT message sent to '%s' (total=%lu)\n", mqtt_topic, messages_sent);
    }
    
    return true;
}

bool sensor_stats(void) {
    time_t now = time(NULL), uptime = now - start_time;
    
    printf("stats: uptime=%lds, messages=%lu, errors=%lu, rate=%.2f/min\n",
           uptime,
           messages_sent,
           read_errors,
           messages_sent > 0 ? (float)messages_sent / ((float) uptime / 60.0f) : 0.0f);
    
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool sensor_config(void) {
    device_path = config_get_string("device-path", DEVICE_PATH_DEFAULT);
    report_period = (time_t)config_get_integer("report-period", REPORT_PERIOD_DEFAULT);
    debug_mode = config_get_bool("debug", false);
    
    printf("sensor: device='%s', period=%lds\n", device_path, report_period);
    
    return true;
}

bool sensor_begin(void) {
    start_time = time(NULL);
    
    device_fp = fopen(device_path, "r");
    if (device_fp == NULL) {
        fprintf(stderr, "sensor: cannot open device '%s'\n", device_path);
        return false;
    }
    
    printf("sensor: device opened successfully\n");
    return true;
}

void sensor_end(void) {
    if (device_fp != NULL) {
        fclose(device_fp);
        device_fp = NULL;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

const struct option config_options[] = {
    {"config", required_argument, 0, 0},        // config
    {"mqtt-client", required_argument, 0, 0},   // mqtt
    {"mqtt-server", required_argument, 0, 0},
    {"mqtt-topic", required_argument, 0, 0},
    {"device-path", required_argument, 0, 0},   // sensor
    {"report-period", required_argument, 0, 0},
    {"debug", required_argument, 0, 0},         // debug
    {0, 0, 0, 0}
};

bool config(const int argc, const char *argv[]) {
    if (!config_load(CONFIG_FILE_DEFAULT, argc, argv, config_options))
        return false;
    
    return mqtt_config() && sensor_config();
}

bool startup(void) {
    return mqtt_begin(&mqttConfig) && sensor_begin();
}

void cleanup(void) {
    sensor_end();
    mqtt_end();
}

bool process(void) {
    static time_t stats_last = 0;
    const time_t STATS_PERIOD = 300; // 5 minutes
    
    if (!sensor_process())
        return false;
    
    if (intervalable(STATS_PERIOD, &stats_last))
        sensor_stats();
    
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define PROCESS_INTERVAL 1  // Check sensor every second
volatile bool running = true;

void signal_handler(const int sig __attribute__((unused))) {
    if (running) {
        printf("stopping\n");
        running = false;
    }
}

int main(const int argc, const char *argv[]) {
    setbuf(stdout, NULL);
    printf("starting\n");
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (!config(argc, argv))
        return EXIT_FAILURE;
    
    if (!startup()) {
        cleanup();
        return EXIT_FAILURE;
    }
    
    while (running) {
        if (!process()) {
            // Device disconnected or error - try to recover
            fprintf(stderr, "sensor: attempting recovery\n");
            sensor_end();
            sleep(5);
            
            if (!sensor_begin()) {
                fprintf(stderr, "sensor: recovery failed, exiting\n");
                cleanup();
                return EXIT_FAILURE;
            }
            
            printf("sensor: recovery successful\n");
        }
        
        for (int i = 0; i < PROCESS_INTERVAL && running; i++)
            sleep(1);
    }
    
    cleanup();
    sensor_stats();
    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
