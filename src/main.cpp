#include <FS.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <WString.h>
#include <Adafruit_NeoPixel.h>
// #include "RunningMedian.h"
#include <RotaryEncoder.h>
#include "secret_defaults.h"

char mqtt_server_address[100];
char mqtt_id[100];
char mqtt_username[100];
char mqtt_password[100];
char mqtt_topic_root[100];

const int leds_pin = D5;
const int switch_pin = D1;
const int rotary_encoder_pin1 = D7;
const int rotary_encoder_pin2 = D6;

char mqtt_topic_mode[100];
char mqtt_topic_color[100];
char mqtt_topic_flash[100];
char mqtt_topic_progress[100];
char mqtt_topic_control[100];
char mqtt_topic_log[100];

const int mqtt_server_port = 1883;

bool shouldSaveConfig = false;

static unsigned long last_connection_attempt = 0;
bool connected = false;

const int num_pixels = 86;

int rainbow_wheel_speed = 20;
static unsigned long last_rainbow_wheel_change = 0;
int rainbow_wheel_pos = 0;
uint32_t rainbowColors[256];

int space_wheel_speed = 1;
static unsigned long last_space_wheel_change = 0;
int space_wheel_pos = 0;

int strobo_off_period = 100;
int strobo_on_period = 8;
static unsigned long last_strobo_change = 0;
bool strobo_state = false;

const int error_wheel_speed = 5;
static unsigned long last_error_wheel_change = 0;
int error_wheel_pos = 0;

int flash_speed = 200;
static unsigned long last_flash_change = 0;
const int start_flash_count = 5;
int flash_count = start_flash_count;
bool flash_state = false;

int current_progress = 0;
int progress_wheel_speed = 20;
static unsigned long last_progress_wheel_change = 0;
int progress_wheel_pos = 0;

const int default_color = 0;
int current_color = default_color;
int flash_color = default_color;

const int MODE_ERROR = 0;
const int MODE_NORMAL = 1;
const int MODE_RAINBOW = 2;
const int MODE_SPACE = 3;
const int MODE_STROBO = 4;
const int MODE_PROGRESS = 5;
const int MODE_FLASH = 6;
const int default_mode = MODE_NORMAL;
const int lowest_mode = MODE_NORMAL;
const int highest_mode = MODE_PROGRESS;
int current_mode = default_mode;
int mode_before_error = current_mode;
int mode_before_flash = current_mode;

const char *RAINBOW_SPEED_CMD = "rs";
const char *SPACE_SPEED_CMD = "sps";
const char *STROBO_SPEED_CMD = "sts";

const int rotary_max = 12;
int rot_last_pos = 0;

int last_switch_triggering = -1;
bool switch_was_triggered = false;

const bool DEBUG_WDT = false;

WiFiClient espClient;
PubSubClient client(espClient);

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(num_pixels, leds_pin, NEO_GRB + NEO_KHZ800);

RotaryEncoder rot_encoder(rotary_encoder_pin1, rotary_encoder_pin2);

DeserializationError read_config_file(DynamicJsonDocument json);
boolean write_config_file(DynamicJsonDocument config_json);
boolean change_single_config_value(char *key, int value);
void write_default_config();
void setup_wifi_and_parameters();
void saveConfigCallback();
void blink(int blinkCount, bool lamp_blink);
void init_mode_change(int new_mode);
void init_color_change(int new_color);
void init_gray_shade(float whiteness);
void mqtt_callback(char *topicChar, byte *payload, unsigned int length);
void setError(bool error_occured);
void reconnect();
void showError(byte WheelPos);
void showRainbow(byte WheelPos);
void showSpace(byte WheelPos);
void showProgress(int progress, int wheel_pos);
void showStrobo(bool stobo_state);
void showRGB(int R, int G, int B);
void showColor(int color);
void handle_rot_encoder();
void calcRainbowColors();

void ICACHE_RAM_ATTR switch_triggered()
{
  long now = millis();
  if (now - last_switch_triggering > 100)
  {
    last_switch_triggering = now;
    switch_was_triggered = true;
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(BUILTIN_LED, OUTPUT);
  pinMode(leds_pin, OUTPUT);
  pinMode(switch_pin, INPUT_PULLUP);
  pinMode(rotary_encoder_pin1, INPUT);
  pinMode(rotary_encoder_pin2, INPUT);

  digitalWrite(BUILTIN_LED, LOW);
  pixels.begin();
  setError(false);
  blink(5, true);

  calcRainbowColors();

  setup_wifi_and_parameters();

  client.setServer(mqtt_server_address, mqtt_server_port);
  client.setCallback(mqtt_callback);

  rot_encoder.setPosition(0);

  // attachInterrupt(digitalPinToInterrupt(switch_pin), switch_triggered, FALLING);

  blink(2, true);

  // write_default_config();
  // while(true) delay(1000);
}

DeserializationError read_config_file(DynamicJsonDocument json)
{
  Serial.println("Try to mount file system to read config parameters");
  if (SPIFFS.begin())
  {
    Serial.println("Successfully mounted file system");
    if (SPIFFS.exists("/config.json"))
    {
      Serial.println("Reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("Opened config.json");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        // DynamicJsonBuffer jsonBuffer;
        // return jsonBuffer.parseObject(buf.get());
        return deserializeJson(json, buf.get());
        /*JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        Serial.println();
        return json;*/
      }
    }
  }
  else
  {
    Serial.println("Failed to mount FS");
  }
  return DeserializationError::InvalidInput;
}

boolean write_config_file(DynamicJsonDocument config_json)
{
  Serial.println("Try to saving config");

  Serial.print("Write: ");
  serializeJson(config_json, Serial);
  Serial.println();

  if (SPIFFS.begin())
  {
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile)
    {
      Serial.println("Failed to open config.json for writing");
    }
    else
    {
      serializeJson(config_json, configFile);
      configFile.close();
      Serial.println("Successfully saved config");
      return true;
    }
  }

  Serial.println("Failed to save config");
  return false;
}

boolean change_single_config_value(char *key, int value)
{
  DynamicJsonDocument config_json(1024);
  DeserializationError error = read_config_file(config_json);
  if (!error)
  {
    config_json[key] = value;
    return write_config_file(config_json);
  }
  else
  {
    Serial.println("Failed to load config.json");
  }
  return false;
}

void write_default_config()
{
  Serial.println("Write default vaules in config");

  DynamicJsonDocument json(1024);

  json["long_workaround_long_workaround_long_workaround"] = "long_workaround_long_workaround_long_workaround";
  json["mqtt_server"] = default_mqtt_server_address;
  json["mqtt_id"] = default_mqtt_id;
  json["mqtt_username"] = default_mqtt_username;
  json["mqtt_password"] = default_mqtt_password;
  json["mqtt_topic_root"] = default_mqtt_topic_root;

  write_config_file(json);

  Serial.println("check...");

  DeserializationError error = read_config_file(json);
  Serial.print("Config: ");
  serializeJson(json, Serial);
  Serial.println();

  strcpy(mqtt_server_address, default_mqtt_server_address);
  strcpy(mqtt_id, default_mqtt_id);
  strcpy(mqtt_username, default_mqtt_username);
  strcpy(mqtt_password, default_mqtt_password);
  strcpy(mqtt_topic_root, default_mqtt_topic_root);
}

void setup_wifi_and_parameters()
{
  DynamicJsonDocument config_json(1024);
  DeserializationError error = read_config_file(config_json);
  Serial.print("Config: ");
  serializeJson(config_json, Serial);
  Serial.println();
  if (!error)
  {
    Serial.println("config.json successfully parsed");

    if (config_json.containsKey("mqtt_server"))
      strcpy(mqtt_server_address, config_json["mqtt_server"]);
    if (config_json.containsKey("mqtt_id"))
      strcpy(mqtt_id, config_json["mqtt_id"]);
    if (config_json.containsKey("mqtt_username"))
      strcpy(mqtt_username, config_json["mqtt_username"]);
    if (config_json.containsKey("mqtt_password"))
      strcpy(mqtt_password, config_json["mqtt_password"]);
    if (config_json.containsKey("mqtt_topic_root"))
      strcpy(mqtt_topic_root, config_json["mqtt_topic_root"]);
  }
  else
  {
    Serial.println("Failed to load config.json. Try to write default config.json.");
    write_default_config();
  }

  WiFi.setAutoReconnect(true);

  WiFiManager wifiManager;

  WiFiManagerParameter mqtt_server_parameter("mqtt_server", "MQTT Server Address", mqtt_server_address, 100);
  wifiManager.addParameter(&mqtt_server_parameter);
  WiFiManagerParameter mqtt_id_parameter("mqtt_id", "MQTT ID", mqtt_id, 100);
  wifiManager.addParameter(&mqtt_id_parameter);
  WiFiManagerParameter mqtt_username_parameter("mqtt_username", "MQTT Username", mqtt_username, 100);
  wifiManager.addParameter(&mqtt_username_parameter);
  WiFiManagerParameter mqtt_password_parameter("mqtt_password", "MQTT Password", mqtt_password, 100);
  wifiManager.addParameter(&mqtt_password_parameter);
  WiFiManagerParameter mqtt_topic_root_parameter("mqtt_topic_root", "MQTT Topic Root", mqtt_topic_root, 100);
  wifiManager.addParameter(&mqtt_topic_root_parameter);

  wifiManager.setSaveConfigCallback(saveConfigCallback);

  Serial.println("Try to connect to Wifi");
  if (!wifiManager.autoConnect("tube_lamp_setup"))
  {
    Serial.println("Failed to connect to Wifi! Restart in 3 seconds.");
    delay(3000);
    ESP.reset();
    delay(5000);
  }
  Serial.println("Successfully connected to Wifi");

  strcpy(mqtt_server_address, mqtt_server_parameter.getValue());
  strcpy(mqtt_id, mqtt_id_parameter.getValue());
  strcpy(mqtt_username, mqtt_username_parameter.getValue());
  strcpy(mqtt_topic_root, mqtt_topic_root_parameter.getValue());

  if (shouldSaveConfig)
  {
    DynamicJsonDocument json(1024);

    json["long_workaround_long_workaround_long_workaround"] = "long_workaround_long_workaround_long_workaround";
    json["mqtt_server"] = mqtt_server_address;
    json["mqtt_id"] = mqtt_id;
    json["mqtt_username"] = mqtt_username;
    json["mqtt_password"] = mqtt_password;
    json["mqtt_topic_root"] = mqtt_topic_root;

    write_config_file(json);
  }

  strcpy(mqtt_topic_mode, mqtt_topic_root);
  strcat(mqtt_topic_mode, "/mode");
  strcpy(mqtt_topic_color, mqtt_topic_root);
  strcat(mqtt_topic_color, "/color");
  strcpy(mqtt_topic_control, mqtt_topic_root);
  strcat(mqtt_topic_control, "/control");
  strcpy(mqtt_topic_log, mqtt_topic_root);
  strcat(mqtt_topic_log, "/log");
  strcpy(mqtt_topic_flash, mqtt_topic_root);
  strcat(mqtt_topic_flash, "/flash");
  strcpy(mqtt_topic_progress, mqtt_topic_root);
  strcat(mqtt_topic_progress, "/progress");
}

void saveConfigCallback()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void blink(int blinkCount, bool lamp_blink)
{
  if (blinkCount > 0)
  {
    digitalWrite(BUILTIN_LED, LOW);
    if (lamp_blink)
    {
      showRGB(10, 10, 10);
    }
    delay(150);
    digitalWrite(BUILTIN_LED, HIGH);
    if (lamp_blink)
    {
      showRGB(0, 0, 0);
    }
    delay(150);
    blink(blinkCount - 1, lamp_blink);
  }
}

void init_mode_change(int new_mode)
{
  // current_mode = new_mode;
  String mode_message(new_mode);
  client.publish(mqtt_topic_mode, mode_message.c_str());
}

void init_color_change(int new_color)
{
  // current_mode = new_color;
  String color_message(new_color);
  client.publish(mqtt_topic_color, color_message.c_str());
}

void init_gray_shade(float whiteness)
{
  int shade = 255 * whiteness;
  int color = shade + shade * 256 + shade * 256 * 256;
  init_color_change(color);
}

void mqtt_callback(char *topicChar, byte *payload, unsigned int length)
{
  Serial.print("Message arrived in topic [");
  Serial.print(topicChar);
  Serial.print("]: ");

  char payloadChar[length + 1];

  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
    payloadChar[i] = (char)payload[i];
  }
  Serial.println();
  payloadChar[length] = 0;

  String topic(topicChar);
  String command(payloadChar);

  if (topic.equals(mqtt_topic_color))
  {
    Serial.println("Change the color of the lamp and set mode to NORMAL");

    current_color = command.toInt();

    String log_message("[COLOR] New color has been set");
    client.publish(mqtt_topic_log, log_message.c_str());

    // current_mode = MODE_NORMAL;
    // String mode_message("1");
    // client.publish(mqtt_topic_mode,mode_message.c_str());
    init_mode_change(MODE_NORMAL);
  }
  if (topic.equals(mqtt_topic_flash))
  {
    Serial.println("Change the flash color and count of the lamp and set mode to FLASH");

    int splitIndex = command.indexOf(" ");
    flash_color = command.substring(0, splitIndex).toInt();
    flash_count = command.substring(splitIndex + 1).toInt();

    String log_message("[FLASH] New flash color and count has been set");
    client.publish(mqtt_topic_log, log_message.c_str());

    if (current_mode != MODE_FLASH)
      mode_before_flash = current_mode;
    init_mode_change(MODE_FLASH);
  }
  if (topic.equals(mqtt_topic_progress))
  {
    Serial.println("Update the current progress value");

    current_progress = command.toInt();

    String log_message("[PROGRESS] New value: ");
    log_message.concat(current_progress);
    client.publish(mqtt_topic_log, log_message.c_str());
  }
  if (topic.equals(mqtt_topic_control))
  {
    Serial.println("New command in control topic arrived");

    if (command.startsWith(RAINBOW_SPEED_CMD))
    {
      command.replace(RAINBOW_SPEED_CMD, "");
      command.remove(0, 1);
      int value = command.toInt();
      if (value > 0)
      {
        rainbow_wheel_speed = value;
        Serial.print("Set rainbow speed to: ");
        Serial.println(rainbow_wheel_speed);
        String log_message("[CTRL] Set rainbow wheelspeed to ");
        log_message.concat(rainbow_wheel_speed);
        client.publish(mqtt_topic_log, log_message.c_str());
      }
      else
      {
        Serial.print("Illegal rainbow speed");
        String log_message("[CTRL] Illegal rainbow wheelspeed");
        client.publish(mqtt_topic_log, log_message.c_str());
      }
    }
    else if (command.startsWith(SPACE_SPEED_CMD))
    {
      command.replace(SPACE_SPEED_CMD, "");
      command.remove(0, 1);
      int value = command.toInt();
      if (value > 0)
      {
        space_wheel_speed = value;
        Serial.print("Set space speed to: ");
        Serial.println(space_wheel_speed);
        String log_message("[CTRL] Set space wheelspeed to ");
        log_message.concat(space_wheel_speed);
        client.publish(mqtt_topic_log, log_message.c_str());
      }
      else
      {
        Serial.print("Illegal space speed");
        String log_message("[CTRL] Illegal space wheelspeed");
        client.publish(mqtt_topic_log, log_message.c_str());
      }
    }
    else if (command.startsWith(STROBO_SPEED_CMD))
    {
      command.replace(STROBO_SPEED_CMD, "");
      command.remove(0, 1);
      int splitIndex = command.indexOf(" ");
      int on_period = command.substring(0, splitIndex).toInt();
      int off_period = command.substring(splitIndex + 1).toInt();
      if (on_period > 0 && off_period > 0)
      {
        strobo_on_period = on_period;
        strobo_off_period = off_period;
        Serial.print("Set strobo on period to: ");
        Serial.println(strobo_on_period);
        Serial.print("Set strobo off period to: ");
        Serial.println(strobo_off_period);
        String log_message("[CTRL] Set strobo periods to ");
        log_message.concat(strobo_on_period);
        log_message.concat(" (on) and ");
        log_message.concat(strobo_off_period);
        log_message.concat(" (off)");
        client.publish(mqtt_topic_log, log_message.c_str());
      }
      else
      {
        Serial.print("Illegal strobo speed");
        String log_message("[CTRL] Illegal strobo speed");
        client.publish(mqtt_topic_log, log_message.c_str());
      }
    }
    else
    {
      Serial.print("Unknown command: ");
      Serial.println(command);
      String log_message("[CMD] Unknown command");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
  }
  else if (topic.equals(mqtt_topic_mode))
  {
    Serial.println("Mode change has been initiated");

    int new_mode = command.toInt();
    switch (new_mode)
    {
    case MODE_ERROR:
    {
      Serial.println("Change the mode of the lamp to ERROR");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to ERROR");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    case MODE_NORMAL:
    {
      Serial.println("Change the mode of the lamp to NORMAL");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to NORMAL");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    case MODE_RAINBOW:
    {
      Serial.println("Change the mode of the lamp to RAINBOW");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to RAINBOW");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    case MODE_SPACE:
    {
      Serial.println("Change the mode of the lamp to SPACE");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to SPACE");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    case MODE_STROBO:
    {
      Serial.println("Change the mode of the lamp to STROBO");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to STROBO");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    case MODE_PROGRESS:
    {
      Serial.println("Change the mode of the lamp to PROGRESS");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to PROGRESS");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    case MODE_FLASH:
    {
      Serial.println("Change the mode of the lamp to FLASH");
      current_mode = new_mode;
      String log_message("[MODE] Mode has been set to FLASH");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    default:
    {
      Serial.println("Mode is not available. Do not change the mode");
      String log_message("[MODE] Mode is not available. Do not change the mode");
      client.publish(mqtt_topic_log, log_message.c_str());
    }
    break;
    }
  }

  // blink(1);
}

void setError(bool error_occured)
{
  if (error_occured)
  {
    Serial.println("Set error mode on");
    if (current_mode != MODE_ERROR)
    {
      mode_before_error = current_mode;
      current_mode = MODE_ERROR;
    }
  }
  else
  {
    Serial.println("Set error mode off");
    if (current_mode == MODE_ERROR)
    {
      current_mode = mode_before_error;
    }
  }
}

void reconnect()
{
  connected = false;
  if (millis() - last_connection_attempt > 5000)
  {
    Serial.println("MQTT client is not connected. Try to reconnect.");
    setError(true);
    Serial.println("Attempting MQTT connection");
    if (client.connect(mqtt_id, mqtt_username, mqtt_password))
    {
      Serial.println("Connected to MQTT server");

      String log_message("[INFO] Connected to MQTT server");
      client.publish(mqtt_topic_log, log_message.c_str());

      Serial.println("Subscribe to control topic");
      if (client.subscribe(mqtt_topic_control))
      {
        Serial.println("Subscribe to color topic");
        if (client.subscribe(mqtt_topic_color))
        {
          Serial.println("Subscribe to mode topic");
          if (client.subscribe(mqtt_topic_mode))
          {
            Serial.println("Subscribe to flash topic");
            if (client.subscribe(mqtt_topic_flash))
            {
              Serial.println("Subscribe to progress topic");
              if (client.subscribe(mqtt_topic_progress))
              {
                setError(false);
                connected = true;
              }
              else
              {
                Serial.print("Failed to subscribe to progress topic, current state = ");
                Serial.println(client.state());
                Serial.println("Try to reconnect in 5 seconds");
              }
            }
            else
            {
              Serial.print("Failed to subscribe to flash topic, current state = ");
              Serial.println(client.state());
              Serial.println("Try to reconnect in 5 seconds");
            }
          }
          else
          {
            Serial.print("Failed to subscribe to mode topic, current state = ");
            Serial.println(client.state());
            Serial.println("Try to reconnect in 5 seconds");
          }
        }
        else
        {
          Serial.print("Failed to subscribe to color topic, current state = ");
          Serial.println(client.state());
          Serial.println("Try to reconnect in 5 seconds");
        }
      }
      else
      {
        Serial.print("Failed to subscribe to control topic, current state = ");
        Serial.println(client.state());
        Serial.println("Try to reconnect in 5 seconds");
      }
    }
    else
    {
      Serial.print("Failed to connect to MQTT server, current state = ");
      Serial.println(client.state());
      Serial.println("Try to reconnect in 5 seconds");
    }
    last_connection_attempt = millis();
  }
}

// Input a value 0 to 255
void showError(byte WheelPos)
{
  uint32_t color;
  if (WheelPos < 127)
  {
    color = pixels.Color(WheelPos, 0, 0);
  }
  else
  {
    color = pixels.Color(255 - WheelPos, 0, 0);
  }
  for (int i = 0; i < num_pixels; i++)
  {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

// Input a value 0 to 255
void showRainbow(byte WheelPos)
{
  for (int i = 0; i < num_pixels; i++)
  {
    pixels.setPixelColor(i, rainbowColors[WheelPos]);
  }
  pixels.show();
}

// Input a value 0 to 255
void showSpace(byte WheelPos)
{
  for (int i = 0; i < num_pixels; i++)
  {
    int interWheelPos = (WheelPos * 2 + i * 256 / num_pixels) % 256;
    pixels.setPixelColor(i, rainbowColors[interWheelPos]);
  }
  pixels.show();
}

void showProgress(int progress, int wheel_pos)
{
  if (progress < 0)
    progress = 0;
  if (progress > 100)
    progress = 100;
  int num_green_leds = num_pixels * double(progress) / 100;
  int wave_pos = 0;
  if (num_green_leds > 0)
    wave_pos = wheel_pos % num_green_leds;
  uint32_t color;
  for (int i = 0; i < num_pixels; i++)
  {
    if (i < num_green_leds)
    {
      double gap_to_wave = wave_pos - i;
      if (gap_to_wave < 0)
        gap_to_wave = num_green_leds + gap_to_wave;
      int green_val = 255 * (1 - gap_to_wave / double(num_pixels));
      color = pixels.Color(0, green_val, 0);
    }
    else
    {
      color = pixels.Color(255, 0, 0);
    }
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void showStrobo(bool stobo_state)
{
  uint32_t color = pixels.Color(0, 0, 0);
  if (stobo_state)
    color = pixels.Color(255, 255, 255);
  for (int i = 0; i < num_pixels; i++)
  {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void showRGB(int R, int G, int B)
{
  if (DEBUG_WDT)
    Serial.println("[Show rgb] Before for loop.");
  for (int i = 0; i < num_pixels; i++)
  {
    pixels.setPixelColor(i, pixels.Color(R, G, B));
  }
  if (DEBUG_WDT)
    Serial.println("[Show rgb] Before pixels show.");
  pixels.show();
}

void showColor(int color)
{
  if (DEBUG_WDT)
    Serial.println("[Show color] Before calc R.");
  int R = color / (256 * 256);
  if (DEBUG_WDT)
    Serial.println("[Show color] Before calc G.");
  int G = (color / 256) % 256;
  if (DEBUG_WDT)
    Serial.println("[Show color] Before calc B.");
  int B = color % 256;
  if (DEBUG_WDT)
    Serial.println("[Show color] Before show rgb.");
  showRGB(R, G, B);
}

void handle_rot_encoder()
{
  rot_encoder.tick();
  int rot_new_pos = rot_encoder.getPosition();
  if (rot_new_pos < 0)
  {
    rot_encoder.setPosition(0);
    rot_new_pos = 0;
  }
  else if (rot_new_pos > rotary_max)
  {
    rot_encoder.setPosition(rotary_max);
    rot_new_pos = rotary_max;
  }
  if (rot_last_pos != rot_new_pos)
  {
    rot_last_pos = rot_new_pos;
    float relative_pos = rot_last_pos / float(rotary_max);
    Serial.print("Rotary encoder is set to ");
    Serial.print((int)relative_pos * 100);
    Serial.println("%");
    init_gray_shade(relative_pos);
  }
}

void calcRainbowColors()
{
  Serial.println("Calculate Rainbow Colors.");
  for (int i = 0; i <= 255; i++)
  {
    // Serial.print(i);
    // Serial.println("/255");
    uint32_t color;
    byte WheelPos = 255 - i;
    if (WheelPos < 85)
    {
      color = pixels.Color(255 - WheelPos * 3, 0, WheelPos * 3, 0);
    }
    else if (WheelPos < 170)
    {
      WheelPos -= 85;
      color = pixels.Color(0, WheelPos * 3, 255 - WheelPos * 3, 0);
    }
    else
    {
      WheelPos -= 170;
      color = pixels.Color(WheelPos * 3, 255 - WheelPos * 3, 0, 0);
    }
    rainbowColors[i] = color;
  }
  /*Serial.print("Rainbow Colors = [");
  for (int i=0;i<=255;i++)
  {
    Serial.print(rainbowColors[i]);
    Serial.print(",");
  }
  Serial.println("]");*/
}

void loop()
{
  if (DEBUG_WDT)
    Serial.println("Start loop.");
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("No connection to Wifi.");
    delay(100);
  }
  // ESP.wdtFeed();

  if (DEBUG_WDT)
    Serial.println("Before test mqtt connection.");
  if (WiFi.status() == WL_CONNECTED && !client.connected())
  {
    if (DEBUG_WDT)
      Serial.println("Before reconnect.");
    reconnect();
  }
  ESP.wdtFeed();

  if (switch_was_triggered)
  {
    Serial.println("Switch triggered.");
    if (!digitalRead(switch_pin))
    {
      int new_mode = current_mode + 1;
      if (new_mode > highest_mode)
        new_mode = lowest_mode;
      Serial.println("Increase mode due to switch triggering.");
      init_mode_change(new_mode);
    }
    else
    {
      Serial.println("False alarm!");
    }
    switch_was_triggered = false;
  }
  // ESP.wdtFeed();

  if (DEBUG_WDT)
    Serial.println("Before switch mode.");
  switch (current_mode)
  {
  case MODE_ERROR:
  {
    if (millis() - last_error_wheel_change > error_wheel_speed)
    {
      if (DEBUG_WDT)
        Serial.println("Error wheel change.");
      if (error_wheel_pos >= 255)
        error_wheel_pos = 0;
      else
        error_wheel_pos++;
      last_error_wheel_change = millis();
      Serial.print("Error pos = ");
      Serial.println(error_wheel_pos);
    }
    if (DEBUG_WDT)
      Serial.println("Before show error.");
    showError(error_wheel_pos);
  }
  break;
  case MODE_NORMAL:
  {
    if (DEBUG_WDT)
      Serial.println("Before show normal.");
    showColor(current_color);
  }
  break;
  case MODE_FLASH:
  {
    if (millis() - last_flash_change > flash_speed)
    {
      if (DEBUG_WDT)
        Serial.println("Flash change.");
      flash_state = !flash_state;
      if (flash_state)
        flash_count--;
      if (flash_count <= 0)
        init_mode_change(mode_before_flash);
      last_flash_change = millis();
      /*Serial.print("flash_state = "); Serial.print(flash_state); Serial.print(" ,");
      Serial.print("flash_count = "); Serial.print(flash_count); Serial.print(" ,");
      Serial.print("last_flash_change = "); Serial.print(last_flash_change); Serial.print(" ,");
      Serial.print("flash_speed = "); Serial.print(flash_speed); Serial.println();*/
    }
    if (DEBUG_WDT)
      Serial.println("Before show flash.");
    if (flash_state)
      showColor(flash_color);
    else
      showColor(0);
  }
  break;
  case MODE_RAINBOW:
  {
    if (millis() - last_rainbow_wheel_change > rainbow_wheel_speed)
    {
      if (DEBUG_WDT)
        Serial.println("Rainbow wheel change.");
      if (rainbow_wheel_pos >= 255)
        rainbow_wheel_pos = 0;
      else
        rainbow_wheel_pos++;
      last_rainbow_wheel_change = millis();
    }
    if (DEBUG_WDT)
      Serial.println("Before show rainbow.");
    showRainbow(rainbow_wheel_pos);
  }
  break;
  case MODE_SPACE:
  {
    if (millis() - last_space_wheel_change > space_wheel_speed)
    {
      if (DEBUG_WDT)
        Serial.println("Space wheel change.");
      if (space_wheel_pos >= 255)
        space_wheel_pos = 0;
      else
        space_wheel_pos++;
      last_space_wheel_change = millis();
    }
    if (DEBUG_WDT)
      Serial.println("Before show space.");
    showSpace(space_wheel_pos);
  }
  break;
  case MODE_STROBO:
  {
    int strobo_speed = strobo_off_period;
    if (strobo_state)
      strobo_speed = strobo_on_period;
    if (millis() - last_strobo_change > strobo_speed)
    {
      if (DEBUG_WDT)
        Serial.println("Strobo change.");
      strobo_state = !strobo_state;
      last_strobo_change = millis();
    }
    if (DEBUG_WDT)
      Serial.println("Before show strobo.");
    showStrobo(strobo_state);
  }
  break;
  case MODE_PROGRESS:
  {
    if (millis() - last_progress_wheel_change > progress_wheel_speed)
    {
      if (DEBUG_WDT)
        Serial.println("Progress wheel change.");
      if (progress_wheel_pos >= 255)
        progress_wheel_pos = 0;
      else
        progress_wheel_pos++;
      last_progress_wheel_change = millis();
    }
    showProgress(current_progress, progress_wheel_pos);
  }
  break;
  }
  // ESP.wdtFeed();

  if (DEBUG_WDT)
    Serial.println("Before handle rot encoder.");
  handle_rot_encoder();
  // ESP.wdtFeed();

  if (DEBUG_WDT)
    Serial.println("Before mqtt loop.");
  if (client.connected())
  {
    client.loop();
  }
  // ESP.wdtFeed();

  if (switch_was_triggered)
  {
    delay(3);
  }
  else
  {
    delay(10);
  }
}