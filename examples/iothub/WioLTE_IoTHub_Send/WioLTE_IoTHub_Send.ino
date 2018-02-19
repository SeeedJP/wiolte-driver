#include <WioLTEforArduino.h>
#include <WioLTEClient.h>
#include <stdio.h>

#define URL "[IoTHub URL]"	//https://[IoTHub address]/devices/[Device ID]/messages/events?api-version=2016-11-14

#define SAS "[SAS Token]"	//SharedAccessSignature sr=[IoTHub Address}%2Fdevices%2F[Device ID]&sig=[Key]&se=[TTL]

#define INTERVAL  (60000)

WioLTE Wio;
WioLTEClient WioClient(&Wio);

void setup() {
  delay(200);

  SerialUSB.println("### I/O Initialize.");
  Wio.Init();

  SerialUSB.println("### Power supply ON.");
  Wio.PowerSupplyLTE(true);
  delay(5000);

  SerialUSB.println("### Turn on or reset.");
  if (!Wio.TurnOnOrReset()) {
    SerialUSB.println("### LTE ERROR! ###");
    return;
  }

  SerialUSB.println("### Connecting to \"soracom.io\".");
  delay(10000);
  if (!Wio.Activate("soracom.io", "sora", "sora")) {
    SerialUSB.println("### Connection ERROR! ###");
    return;
  }

  SerialUSB.println("### SSL Setting ###");
  delay(5000);

  if (!Wio.SetSSLCFG(URL)){
    SerialUSB.println("### SSL Config ERROR! ###");
    return;
  }
  
}

void loop() {
  char data[100];
  int status;
  
  SerialUSB.println("### Post.");
  sprintf(data, "{\"value1\":\"uptime %lu\"}", millis() / 1000);
  SerialUSB.print("Post:");
  SerialUSB.print(data);
  SerialUSB.println("");
  
  if (!Wio.IoTHubSend(URL, data, SAS, &status)) {
    SerialUSB.println("### ERROR! ###");
    goto err;
  }
  SerialUSB.print("Status:");
  SerialUSB.println(status);
  
err:
  SerialUSB.println("### Wait.");
  delay(INTERVAL);

}
