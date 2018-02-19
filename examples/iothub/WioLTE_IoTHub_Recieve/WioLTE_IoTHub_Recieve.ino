#include <WioLTEforArduino.h>
#include <WioLTEClient.h>
#include <stdio.h>
#include <string.h>

#define URL "[IoTHub URL]"	//https://[IoTHub address]/devices/[Device ID]/messages/deviceBound?api-version=2016-11-14

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
  char data[512];
  int dataSize=512;
  int r,g,b;

  if(Wio.IoTHubRecieve(URL,data,SAS,dataSize)>0){
    SerialUSB.println("### Get.");
    //SerialUSB.println("Recieve Data:");
    SerialUSB.println("---------------------------------------------------------");
    SerialUSB.println(data);
    SerialUSB.println("---------------------------------------------------------");

    if(!strcmp(data,"red")) {
      SerialUSB.println("red");
      r=255;
      g=0;
      b=0;
    }
    else if (!strcmp(data,"green")) {
      SerialUSB.println("green");
      r=0;
      g=255;
      b=0;
    }
    else if (!strcmp(data,"blue")) {
      SerialUSB.println("blue");
      r=0;
      g=0;
      b=255;
    }
    else if (!strcmp(data,"white")) {
      SerialUSB.println("white");
      r=255;
      g=255;
      b=255;
    }
    else {
      r=0;
      g=0;
      b=0;
    }

    Wio.LedSetRGB(r,g,b);

  }

  

  delay(30000);
}
