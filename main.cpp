#include "mbed.h"
#include "math.h"
#include <iomanip>
#include <cmath>
#include "uLCD_4DGL.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"
#include "TextLCD.h"
#include "mbed_rpc.h"
#include "accelerometer_handler.h"
#include "config.h"
#include "magic_wand_model_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"
#include "stm32l475e_iot01_accelero.h"
void gestureUI(Arguments *in, Reply *out);
void tilt(Arguments *in, Reply *out);
RPCFunction rpcgesture(&gestureUI, "gestureUI");
RPCFunction rpctilt(&tilt, "tilt");
BufferedSerial pc(USBTX, USBRX);
Thread thread, t;
EventQueue queue(64 * EVENTS_EVENT_SIZE);
InterruptIn sw0(USER_BUTTON);
WiFiInterface *wifi;

int16_t PDataXYZ[3] = {0};
int16_t rDataXYZ[3] = {0};
uLCD_4DGL uLCD(D1, D0, D2);
DigitalOut myled(LED1);
DigitalOut myled2(LED2);
DigitalOut myled3(LED3);

int Count = 0;
int ThresholdCount = 10;

int off = 1;
int off2 = 1;

double val = 0;
int idR[32] = {0};
int indexR = 0;
//int gesture_index;
int c=0, angle = 0;
constexpr int kTensorArenaSize = 60 * 1024;
uint8_t tensor_arena[kTensorArenaSize];
MQTT::Client<MQTTNetwork, Countdown> *rpcclient;
volatile int message_num = 0;
volatile int arrivedcount = 0;
volatile bool closed = false;

const char* topic = "Mbed";

Thread mqtt_thread(osPriorityHigh);
EventQueue mqtt_queue;

void led() {
  while (off2) {
    myled=!myled;
    ThisThread::sleep_for(500ms);
  }
}

void Confirm_print() {
  uLCD.cls();
   printf("\nConfirm ! %d\n", c);
   uLCD.printf("\nConfirm! ! !\n");
   uLCD.printf("\nFinal Threshold angle = %d\n",angle );
   myled = 0;
}

void Confirm_angle() {
   c=1;
   //printf("c = %d", c);
   mqtt_queue.call(&Confirm_print);
}

int PredictGesture(float* output) {
  // How many times the most recent gesture has been matched in a row
  static int continuous_count = 0;
  // The result of the last prediction
  static int last_predict = -1;
  // Find whichever output has a probability > 0.8 (they sum to 1)
  int this_predict = -1;
  for (int i = 0; i < label_num; i++) {
    if (output[i] > 0.8) this_predict = i;
  }

  // No gesture was detected above the threshold
  if (this_predict == -1) {
    continuous_count = 0;
    last_predict = label_num;
    return label_num;
  }

  if (last_predict == this_predict) {
    continuous_count += 1;
  } else {
    continuous_count = 0;
  }
  last_predict = this_predict;

  // If we haven't yet had enough consecutive matches for this gesture,
  // report a negative result
  if (continuous_count < config.consecutiveInferenceThresholds[this_predict]) {
    return label_num;
  }
  // Otherwise, we've seen a positive result, so clear all our variables
  // and report it
  continuous_count = 0;
  last_predict = -1;

  return this_predict;
}


// I2C Communication
I2C i2c_lcd(D14,D15); // SDA, SCL

TextLCD_I2C lcd(&i2c_lcd, 0x4E, TextLCD::LCD16x2);  // I2C bus, PCF8574 Slaveaddress, LCD Type

void publish_message(MQTT::Client<MQTTNetwork, Countdown>* client) {
    message_num++;
    MQTT::Message message;
    char buff[100];
    sprintf(buff, "QoS0 Hello, Python! #%d", message_num);
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
    message.payload = (void*) buff;
    message.payloadlen = strlen(buff) + 1;
    int rc = client->publish(topic, message);

    printf("rc:  %d\r\n", rc);
    printf("Puslish message: %s\r\n", buff);
}

void angle_select() {
   // Whether we should clear the buffer next time we fetch data
  bool should_clear_buffer = false;
  bool got_data = false;

  // The gesture index of the prediction
  int gesture_index;

  // Set up logging.
  static tflite::MicroErrorReporter micro_error_reporter;
  tflite::ErrorReporter* error_reporter = &micro_error_reporter;

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  const tflite::Model* model = tflite::GetModel(g_magic_wand_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    error_reporter->Report(
        "Model provided is schema version %d not equal "
        "to supported version %d.",
        model->version(), TFLITE_SCHEMA_VERSION);
    return ;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  static tflite::MicroOpResolver<6> micro_op_resolver;
  micro_op_resolver.AddBuiltin(
      tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
      tflite::ops::micro::Register_DEPTHWISE_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_MAX_POOL_2D,
                               tflite::ops::micro::Register_MAX_POOL_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_CONV_2D,
                               tflite::ops::micro::Register_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_FULLY_CONNECTED,
                               tflite::ops::micro::Register_FULLY_CONNECTED());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_SOFTMAX,
                               tflite::ops::micro::Register_SOFTMAX());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_RESHAPE,
                               tflite::ops::micro::Register_RESHAPE(), 1);

  // Build an interpreter to run the model with
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
  tflite::MicroInterpreter* interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors
  interpreter->AllocateTensors();

  // Obtain pointer to the model's input tensor
  TfLiteTensor* model_input = interpreter->input(0);
  if ((model_input->dims->size != 4) || (model_input->dims->data[0] != 1) ||
      (model_input->dims->data[1] != config.seq_length) ||
      (model_input->dims->data[2] != kChannelNumber) ||
      (model_input->type != kTfLiteFloat32)) {
    error_reporter->Report("Bad input tensor parameters in model");
    return ;
  }

  int input_length = model_input->bytes / sizeof(float);

  TfLiteStatus setup_status = SetupAccelerometer(error_reporter);
  if (setup_status != kTfLiteOk) {
    error_reporter->Report("Set up failed\n");
    return ;
  }

  error_reporter->Report("Set up successful...\n");

  while (true) {
    // Attempt to read new data from the accelerometer
    got_data = ReadAccelerometer(error_reporter, model_input->data.f,
                                 input_length, should_clear_buffer);

    // If there was no new data,
    // don't try to clear the buffer again and wait until next time
    if (!got_data) {
      should_clear_buffer = false;
      continue;
    }

    // Run inference, and report any error
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
      error_reporter->Report("Invoke failed on index: %d\n", begin_index);
      continue;
    }

    // Analyze the results to obtain a prediction
    gesture_index = PredictGesture(interpreter->output(0)->data.f);

    // Clear the buffer next time we read data
    should_clear_buffer = gesture_index < label_num;

    sw0.rise(&Confirm_angle);
    // Produce an output
    if (gesture_index < label_num) {
      error_reporter->Report(config.output_message[gesture_index]);
      if (gesture_index == 0) {
        uLCD.cls();
        uLCD.printf("\nThreshold angle = 30\n");
        angle = 30;
      }
      else if (gesture_index == 1) {
        uLCD.cls();
        uLCD.printf("\nThreshold angle = 35\n");
        angle = 35;
      }
      else if (gesture_index == 2) {
        uLCD.cls();
        uLCD.printf("\nThreshold angle = 40\n");
        angle = 40;
      }
    }

    if (c == 1) {
      //printf("hello");
      mqtt_queue.call(&publish_message, rpcclient);
      return;
    }
  }
}

void messageArrived(MQTT::MessageData& md) {
    MQTT::Message &message = md.message;
    char msg[300];
    sprintf(msg, "Message arrived: QoS%d, retained %d, dup %d, packetID %d\r\n", message.qos, message.retained, message.dup, message.id);
    printf(msg);
    ThisThread::sleep_for(1000ms);
    char payload[300];
    sprintf(payload, "Payload %.*s\r\n", message.payloadlen, (char*)message.payload);
    printf(payload);
    ++arrivedcount;
    if (c == 1) {
      off2 = 0; 
      printf("Confirm angle");
      c = 0;
      queue.call(led);
    }
    if (Count > ThresholdCount) {
      Count = 0;
      off = 0;
    }
}

void close_mqtt() {
    closed = true;
}

void mqtt() {
  wifi = WiFiInterface::get_default_instance();
    if (!wifi) {
            printf("ERROR: No WiFiInterface found.\r\n");
            return;
    }


    printf("\nConnecting to %s...\r\n", MBED_CONF_APP_WIFI_SSID);
    int ret = wifi->connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD, NSAPI_SECURITY_WPA_WPA2);
    if (ret != 0) {
            printf("\nConnection error: %d\r\n", ret);
            return;
    }


    NetworkInterface* net = wifi;
    MQTTNetwork mqttNetwork(net);
    MQTT::Client<MQTTNetwork, Countdown> client(mqttNetwork);
    rpcclient = &client;

    //TODO: revise host to your IP
    const char* host = "172.20.10.3";
    printf("Connecting to TCP network...\r\n");

    SocketAddress sockAddr;
    sockAddr.set_ip_address(host);
    sockAddr.set_port(1883);

    printf("address is %s/%d\r\n", (sockAddr.get_ip_address() ? sockAddr.get_ip_address() : "None"),  (sockAddr.get_port() ? sockAddr.get_port() : 0) ); //check setting

    int rc = mqttNetwork.connect(sockAddr);//(host, 1883);
    if (rc != 0) {
            printf("Connection error.");
            return;
    }
    printf("Successfully connected!\r\n");

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    data.clientID.cstring = "Mbed";

    if ((rc = client.connect(data)) != 0){
            printf("Fail to connect MQTT\r\n");
    }
    if (client.subscribe(topic, MQTT::QOS0, messageArrived) != 0){
            printf("Fail to subscribe\r\n");
    }

    mqtt_thread.start(callback(&mqtt_queue, &EventQueue::dispatch_forever));
    
    //sw0.rise(mqtt_queue.event(&Confirm_angle));
    //sw0.rise(mqtt_queue.event(&publish_message, &client));
    //btn3.rise(&close_mqtt);

    int num = 0;
    while (num != 5) {
            client.yield(100);
            ++num;
    }

    while (1) {
            if (closed) break;
            client.yield(500);
            ThisThread::sleep_for(500ms);
    }

    printf("Ready to close MQTT Network......\n");

    if ((rc = client.unsubscribe(topic)) != 0) {
            printf("Failed: rc from unsubscribe was %d\n", rc);
    }
    if ((rc = client.disconnect()) != 0) {
    printf("Failed: rc from disconnect was %d\n", rc);
    }

    mqttNetwork.disconnect();
    printf("Successfully closed!\n");

    return;
}

int main() {
   lcd.setCursor(TextLCD::CurOff_BlkOn);
   BSP_ACCELERO_Init();
   char buf[256], outbuf[256];
   FILE *devin = fdopen(&pc, "r");
   FILE *devout = fdopen(&pc, "w");
   t.start(callback(&queue, &EventQueue::dispatch_forever));
   queue.call(mqtt);
   while(1) {
      memset(buf, 0, 256);
      for (int i = 0; ; i++) {
          char recv = fgetc(devin);
          if (recv == '\n') {
              printf("\r\n");
              break;
          }
          buf[i] = fputc(recv, devout);
      }
      //Call the static call method on the RPC class
      RPC::call(buf, outbuf);
      printf("%s\r\n", outbuf);
   }
}

void record(void) {
   //double val;
   BSP_ACCELERO_AccGetXYZ(PDataXYZ);
   val = PDataXYZ[0]*rDataXYZ[0] + PDataXYZ[1]*rDataXYZ[1] + PDataXYZ[2]*rDataXYZ[2];
   val = val / sqrt(PDataXYZ[0]*PDataXYZ[0] + PDataXYZ[1]*PDataXYZ[1] + PDataXYZ[2]*PDataXYZ[2]);
   val = val / sqrt(rDataXYZ[0]*rDataXYZ[0] + rDataXYZ[1]*rDataXYZ[1] + rDataXYZ[2]*rDataXYZ[2]);
   val = acos(val);
   val = val/ M_PI * 180;
   uLCD.cls();
   uLCD.printf("angle = %g  %d, %d, %d\n", val, PDataXYZ[0], PDataXYZ[1], PDataXYZ[2]);
   if (val > angle) {
    printf("angle = %g  %d, %d, %d\n", val, PDataXYZ[0], PDataXYZ[1], PDataXYZ[2]);
    mqtt_queue.call(&publish_message, rpcclient);
    Count = Count + 1;
   }
}

void initialize() {
  BSP_ACCELERO_AccGetXYZ(rDataXYZ);
  printf("%d, %d, %d\n", rDataXYZ[0], rDataXYZ[1], rDataXYZ[2]);
}

void tilt(Arguments *in, Reply *out) {
  printf("Tilt mode");
  myled2 = 1;
  myled3 = 0;
  off = 1;
  c = 0;
  message_num = 0;
  sw0.rise(&Confirm_angle);
  while (c == 0) {
    myled3 = !myled3;
    idR[indexR++] = mqtt_queue.call(initialize);
    indexR = indexR % 32;
    ThisThread::sleep_for(100ms);
  }
  myled3 = 0;
  while (off) {
    idR[indexR++] = mqtt_queue.call(record);
    indexR = indexR % 32;
    ThisThread::sleep_for(100ms);
  }
}

void gestureUI(Arguments *in, Reply *out) {
   //const char *tmp = in->getArg<const char*>();
   myled = 1;
   off2 = 1;
   printf("GESTURE MODE");
   c = 0;
   mqtt_queue.call(angle_select);
   //for(int i = 0; tmp[i] != 0; i++) {
     // lcd.putc(tmp[i]);
   //}
}



/*void doLocate(Arguments *in, Reply *out) {
   int x = in->getArg<int>();
   int y = in->getArg<int>();
   lcd.locate(x,y);
   printf("locate (col,row)=(%d,%d)", x, y);
}*/