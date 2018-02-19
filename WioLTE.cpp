#include <Arduino.h>
#include <stdio.h>
#include <limits.h>
#include "WioLTEforArduino.h"

#ifdef WIOLTE_DEBUG
#define DEBUG_PRINT(str)			DebugPrint(str)
#define DEBUG_PRINTLN(str)			DebugPrintln(str)
static void DebugPrint(const char* str)
{
	for (int i = 0; i < strlen(str); i++) SerialUSB.print(str[i]);
}
static void DebugPrintln(const char* str)
{
	DebugPrint(str);
	DebugPrint("\r\n");
}
#else
#define DEBUG_PRINT(str)
#define DEBUG_PRINTLN(str)
#endif // WIOLTE_DEBUG

#define RET_OK(val)					(ReturnOk(val))
#define RET_ERR(val,err)			(ReturnError(__LINE__, val, err))

#define CONNECT_ID_NUM				(12)
#define POLLING_INTERVAL			(100)

#define HTTP_POST_USER_AGENT		"QUECTEL_MODULE"
#define HTTP_POST_CONTENT_TYPE		"application/json"

#define LINEAR_SCALE(val, inMin, inMax, outMin, outMax)	(((val) - (inMin)) / ((inMax) - (inMin)) * ((outMax) - (outMin)) + (outMin))

////////////////////////////////////////////////////////////////////////////////////////
// Helper functions

static void PinModeAndDefault(int pin, WiringPinMode mode)
{
	pinMode(pin, mode);
}

static void PinModeAndDefault(int pin, WiringPinMode mode, int value)
{
	pinMode(pin, mode);
	if (mode == OUTPUT) digitalWrite(pin, value);
}

static int HexToInt(char c)
{
	if ('0' <= c && c <= '9') return c - '0';
	else if ('a' <= c && c <= 'f') return c - 'a' + 10;
	else if ('A' <= c && c <= 'F') return c - 'A' + 10;
	else return -1;
}

static bool ConvertHexToBytes(const char* hex, byte* data, int dataSize)
{
	int high;
	int low;

	for (int i = 0; i < dataSize; i++) {
		high = HexToInt(hex[i * 2]);
		low = HexToInt(hex[i * 2 + 1]);
		if (high < 0 || low < 0) return false;
		data[i] = high * 16 + low;
	}

	return true;
}

static int NumberOfDigits(int value)
{
	int digits = 0;

	if (value < 0) {
		digits++;
		value *= -1;
	}

	do {
		digits++;
		value /= 10;
	} while (value > 0);

	return digits;
}

static bool SplitUrl(const char* url, const char** host, int* hostLength, const char** uri, int* uriLength)
{
	if (strncmp(url, "http://", 7) == 0) {
		*host = &url[7];
	}
	else if (strncmp(url, "https://", 8) == 0) {
		*host = &url[8];
	}
	else {
		return false;
	}

	const char* ptr;
	for (ptr = *host; *ptr != '\0'; ptr++) {
		if (*ptr == '/') break;
	}
	*hostLength = ptr - *host;
	*uri = ptr;
	*uriLength = strlen(ptr);

	return true;
}

static bool SmAddressFieldToString(const byte* addressField, char* str, int strSize)
{
	byte addressLength = addressField[0];
	byte typeOfAddress = addressField[1];
	const byte* addressValue = &addressField[2];

	if (addressLength + 1 > strSize) return false;

	for (int i = 0; i < addressLength; i++)
	{
		if (i % 2 == 0)
		{
			str[i] = '0' + (addressValue[i / 2] & 0x0f);
		}
		else
		{
			str[i] = '0' + (addressValue[i / 2] >> 4);
		}
	}
	str[addressLength] = '\0';

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// WioLTE

bool WioLTE::ReturnError(int lineNumber, bool value, WioLTE::ErrorCodeType errorCode)
{
	_LastErrorCode = errorCode;

	char str[100];
	sprintf(str, "%d", lineNumber);
	DEBUG_PRINT("ERROR! ");
	DEBUG_PRINTLN(str);

	return value;
}

int WioLTE::ReturnError(int lineNumber, int value, WioLTE::ErrorCodeType errorCode)
{
	_LastErrorCode = errorCode;

	char str[100];
	sprintf(str, "%d", lineNumber);
	DEBUG_PRINT("ERROR! ");
	DEBUG_PRINTLN(str);

	return value;
}

bool WioLTE::IsBusy() const
{
	return digitalRead(STATUS_PIN) ? true : false;
}

bool WioLTE::Reset()
{
	digitalWrite(RESET_MODULE_PIN, LOW);
	delay(200);
	digitalWrite(RESET_MODULE_PIN, HIGH);
	delay(300);

	Stopwatch sw;
	sw.Start();
	while (_Module.WaitForResponse("RDY", 100) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return false;
	}
	DEBUG_PRINTLN("");

	return true;
}

bool WioLTE::TurnOn()
{
	delay(100);
	digitalWrite(PWR_KEY_PIN, HIGH);
	delay(200);
	digitalWrite(PWR_KEY_PIN, LOW);

	Stopwatch sw;
	sw.Start();
	while (IsBusy()) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 5000) return false;
		delay(100);
	}
	DEBUG_PRINTLN("");

	sw.Start();
	while (_Module.WaitForResponse("RDY", 100) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return false;
	}
	DEBUG_PRINTLN("");

	return true;
}

int WioLTE::GetFirstIndexOfReceivedSMS()
{
	const char* parameter;
	const char* hex;
	ArgumentParser parser;

	if (_Module.WriteCommandAndWaitForResponse("AT+CMGF=0", "OK", 500) == NULL) return -1;

	_Module.WriteCommand("AT+CMGL=4");	// ALL

	int messageIndex = -1;
	while (true) {
		parameter = _Module.WaitForResponse("OK", 500, "+CMGL: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH));
		if (parameter == NULL) return -1;
		if (strcmp(parameter, "OK") == 0) break;
		if (messageIndex < 0) {
			parser.Parse(parameter);
			if (parser.Size() != 4) return -1;
			messageIndex = atoi(parser[0]);
		}

		const char* hex = _Module.WaitForResponse(NULL, 500, "");
		if (hex == NULL) return -1;
	}

	return messageIndex < 0 ? -2 : messageIndex;
}

bool WioLTE::HttpSetUrl(const char* url)
{
	StringBuilder str;
	if (!str.WriteFormat("AT+QHTTPURL=%d", strlen(url))) return false;
	_Module.WriteCommand(str.GetString());
	if (_Module.WaitForResponse("CONNECT", 500) == NULL) return false;

	_Module.Write(url);
	if (_Module.WaitForResponse("OK", 500) == NULL) return false;

	return true;
}

WioLTE::WioLTE() : _Module(), _Led(1, RGB_LED_PIN), _LastErrorCode(E_OK)
{
}

WioLTE::ErrorCodeType WioLTE::GetLastError() const
{
	return _LastErrorCode;
}

void WioLTE::Init()
{
	// Power supply
	PinModeAndDefault(MODULE_PWR_PIN, OUTPUT, LOW);
	PinModeAndDefault(ANT_PWR_PIN, OUTPUT, LOW);
	PinModeAndDefault(ENABLE_VCCB_PIN, OUTPUT, LOW);
#if defined WIOLTE_SCHEMATIC_B
	PinModeAndDefault(RGB_LED_PWR_PIN, OUTPUT, HIGH);
	PinModeAndDefault(SD_POWR_PIN, OUTPUT, LOW);
#endif // WIOLTE_SCHEMATIC_B

	// Turn on/off Pins
	PinModeAndDefault(PWR_KEY_PIN, OUTPUT, LOW);
	PinModeAndDefault(RESET_MODULE_PIN, OUTPUT, HIGH);

	// Status Indication Pins
	PinModeAndDefault(STATUS_PIN, INPUT);

	// UART Interface
	PinModeAndDefault(DTR_PIN, OUTPUT, LOW);

	// GPIO Pins
	PinModeAndDefault(WAKEUP_IN_PIN, OUTPUT, LOW);
	PinModeAndDefault(W_DISABLE_PIN, OUTPUT, HIGH);
	//PinModeAndDefault(AP_READY_PIN, OUTPUT);  // NOT use
  
	_Module.Init();
	_Led.begin();
	_LastErrorCode = E_OK;
}

void WioLTE::LedSetRGB(byte red, byte green, byte blue)
{
	_Led.WS2812SetRGB(0, red, green, blue);
	_Led.WS2812Send();
	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplyLTE(bool on)
{
	digitalWrite(MODULE_PWR_PIN, on ? HIGH : LOW);
	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplyGNSS(bool on)
{
	digitalWrite(ANT_PWR_PIN, on ? HIGH : LOW);
	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplyGrove(bool on)
{
	digitalWrite(ENABLE_VCCB_PIN, on ? HIGH : LOW);
	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplySD(bool on)
{
#if defined WIOLTE_SCHEMATIC_B
	digitalWrite(SD_POWR_PIN, on ? HIGH : LOW);
#endif // WIOLTE_SCHEMATIC_B
	_LastErrorCode = E_OK;
}

bool WioLTE::TurnOnOrReset()
{
	if (!IsBusy()) {
		if (!Reset()) return RET_ERR(false, E_UNKNOWN);
	}
	else {
		if (!TurnOn()) return RET_ERR(false, E_UNKNOWN);
	}

	Stopwatch sw;
	sw.Start();
	while (_Module.WriteCommandAndWaitForResponse("AT", "OK", 500) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return RET_ERR(false, E_UNKNOWN);
	}
	DEBUG_PRINTLN("");

	if (_Module.WriteCommandAndWaitForResponse("ATE0", "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse("AT+QURCCFG=\"urcport\",\"uart1\"", "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
	_Module.WriteCommand("AT+QSCLK=1");
	if (_Module.WaitForResponse("OK", 500, "ERROR") == NULL) return RET_ERR(false, E_UNKNOWN);

	sw.Start();
	while (true) {
		_Module.WriteCommand("AT+CPIN?");
		const char* response = _Module.WaitForResponse("OK", 5000, "+CME ERROR: ", ModuleSerial::WFR_START_WITH);
		if (response == NULL) return RET_ERR(false, E_UNKNOWN);
		if (strcmp(response, "OK") == 0) break;
		if (sw.ElapsedMilliseconds() >= 10000) return RET_ERR(false, E_UNKNOWN);
		delay(POLLING_INTERVAL);
	}

	return RET_OK(true);
}

bool WioLTE::TurnOff()
{
	if (_Module.WriteCommandAndWaitForResponse("AT+QPOWD", "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WaitForResponse("POWERED DOWN", 60000) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

bool WioLTE::Sleep()
{
	digitalWrite(DTR_PIN, HIGH);

	return RET_OK(true);
}

bool WioLTE::Wakeup()
{
	const char* parameter;

	digitalWrite(DTR_PIN, LOW);

	Stopwatch sw;
	sw.Start();
	while (_Module.WriteCommandAndWaitForResponse("AT", "OK", 500) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 2000) return RET_ERR(false, E_UNKNOWN);
	}
	DEBUG_PRINTLN("");

	return RET_OK(true);
}

int WioLTE::GetIMEI(char* imei, int imeiSize)
{
	const char* response;
	std::vector<char> preResponse;

	_Module.WriteCommand("AT+GSN");
	while (true) {
		if ((response = _Module.WaitForResponse(NULL, 500, "")) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (strcmp(response, "OK") == 0) break;
		preResponse.resize(strlen(response));
		memcpy(&preResponse[0], response, preResponse.size());
	}

	if (imeiSize < preResponse.size() + 1) return RET_ERR(-1, E_UNKNOWN);
	memcpy(imei, &preResponse[0], preResponse.size());
	imei[preResponse.size()] = '\0';

	return RET_OK((int)preResponse.size());
}

int WioLTE::GetIMSI(char* imsi, int imsiSize)
{
	const char* response;
	std::vector<char> preResponse;

	_Module.WriteCommand("AT+CIMI");
	while (true) {
		if ((response = _Module.WaitForResponse(NULL, 500, "")) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (strcmp(response, "OK") == 0) break;
		preResponse.resize(strlen(response));
		memcpy(&preResponse[0], response, preResponse.size());
	}

	if (imsiSize < preResponse.size() + 1) return RET_ERR(-1, E_UNKNOWN);
	memcpy(imsi, &preResponse[0], preResponse.size());
	imsi[preResponse.size()] = '\0';

	return RET_OK((int)preResponse.size());
}

int WioLTE::GetPhoneNumber(char* number, int numberSize)
{
	const char* parameter;
	ArgumentParser parser;

	_Module.WriteCommand("AT+CNUM");
	bool set = false;
	while (true) {
		if ((parameter = _Module.WaitForResponse("OK", 500, "+CNUM: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (strcmp(parameter, "OK") == 0) break;

		if (set) continue;

		parser.Parse(parameter);
		if (parser.Size() < 2) return RET_ERR(-1, E_UNKNOWN);
		if (numberSize < strlen(parser[1]) + 1) RET_ERR(-1, E_UNKNOWN);
		strcpy(number, parser[1]);
		set = true;
	}
	if (!set) RET_OK(0);

	return RET_OK((int)strlen(number));
}

int WioLTE::GetReceivedSignalStrength()
{
	const char* parameter;

	_Module.WriteCommand("AT+CSQ");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CSQ: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(INT_MIN, E_UNKNOWN);

	ArgumentParser parser;
	parser.Parse(parameter);
	if (parser.Size() != 2) return RET_ERR(INT_MIN, E_UNKNOWN);
	int rssi = atoi(parser[0]);

	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(INT_MIN, E_UNKNOWN);

	if (rssi == 0) return RET_OK(-113);
	else if (rssi == 1) return RET_OK(-111);
	else if (2 <= rssi && rssi <= 30) return RET_OK((int)LINEAR_SCALE((double)rssi, 2, 30, -109, -53));
	else if (rssi == 31) return RET_OK(-51);
	else if (rssi == 99) return RET_OK(-999);
	else if (rssi == 100) return RET_OK(-116);
	else if (rssi == 101) return RET_OK(-115);
	else if (102 <= rssi && rssi <= 190) return RET_OK((int)LINEAR_SCALE((double)rssi, 102, 190, -114, -26));
	else if (rssi == 191) return RET_OK(-25);
	else if (rssi == 199) return RET_OK(-999);
	
	return RET_OK(-999);
}

bool WioLTE::GetTime(struct tm* tim)
{
	const char* parameter;

	_Module.WriteCommand("AT+CCLK?");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CCLK: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);

	if (strlen(parameter) != 22) return RET_ERR(false, E_UNKNOWN);
	if (parameter[0] != '"') return RET_ERR(false, E_UNKNOWN);
	if (parameter[3] != '/') return RET_ERR(false, E_UNKNOWN);
	if (parameter[6] != '/') return RET_ERR(false, E_UNKNOWN);
	if (parameter[9] != ',') return RET_ERR(false, E_UNKNOWN);
	if (parameter[12] != ':') return RET_ERR(false, E_UNKNOWN);
	if (parameter[15] != ':') return RET_ERR(false, E_UNKNOWN);
	if (parameter[21] != '"') return RET_ERR(false, E_UNKNOWN);

	int yearOffset = atoi(&parameter[1]);
	tim->tm_year = (yearOffset >= 80 ? 1900 : 2000) + yearOffset - 1900;
	tim->tm_mon = atoi(&parameter[4]) - 1;
	tim->tm_mday = atoi(&parameter[7]);
	tim->tm_hour = atoi(&parameter[10]);
	tim->tm_min = atoi(&parameter[13]);
	tim->tm_sec = atoi(&parameter[16]);
	tim->tm_wday = 0;
	tim->tm_yday = 0;
	tim->tm_isdst = -1;

	return RET_OK(true);
}

//bool WioLTE::Call(const char* dialNumber)
//{
//
//	char* str = (char*)alloca(3 + strlen(dialNumber) + 1);
//	sprintf(str, "ATD%s", dialNumber);
//	_Module.WriteCommand(str);
//
//	const char* response;
//	do {
//		response = _Module.WaitForResponse(NULL, 5000, "");
//		if (strcmp(response, "NO DIALTONE") == 0) return RET_ERR(false, E_UNKNOWN);
//		if (strcmp(response, "BUSY"       ) == 0) return RET_ERR(false, E_UNKNOWN);
//		if (strcmp(response, "NO CARRIER" ) == 0) return RET_ERR(false, E_UNKNOWN);
//	} while (strcmp(response, "OK") != 0);
//
//	return RET_OK(true);
//}
//
//bool WioLTE::HangUp()
//{
//	if (_Module.WriteCommandAndWaitForResponse("ATH", "OK", 90000) == NULL) return RET_ERR(false, E_UNKNOWN);
//
//	return RET_OK(true);
//}

bool WioLTE::SendSMS(const char* dialNumber, const char* message)
{
	if (_Module.WriteCommandAndWaitForResponse("AT+CMGF=1", "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+CMGS=\"%s\"", dialNumber)) return RET_ERR(false, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	if (_Module.WaitForResponse(NULL, 500, "> ", ModuleSerial::WFR_WITHOUT_DELIM) == NULL) return RET_ERR(false, E_UNKNOWN);
	_Module.Write(message);
	_Module.Write("\x1a");
	if (_Module.WaitForResponse("OK", 120000) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

int WioLTE::ReceiveSMS(char* message, int messageSize, char* dialNumber, int dialNumberSize)
{
	const char* parameter;
	const char* hex;

	int messageIndex = GetFirstIndexOfReceivedSMS();
	if (messageIndex == -2) return RET_OK(0);
	if (messageIndex < 0) return RET_ERR(-1, E_UNKNOWN);

	if (_Module.WriteCommandAndWaitForResponse("AT+CMGF=0", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+CMGR=%d", messageIndex)) return RET_ERR(-1, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());

	parameter = _Module.WaitForResponse(NULL, 500, "+CMGR: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH));
	if (parameter == NULL) return RET_ERR(-1, E_UNKNOWN);

	hex = _Module.WaitForResponse(NULL, 500, "");
	if (hex == NULL) return RET_ERR(-1, E_UNKNOWN);
	int hexSize = strlen(hex);
	if (hexSize % 2 != 0) return RET_ERR(-1, E_UNKNOWN);
	int dataSize = hexSize / 2;
	byte* data = (byte*)alloca(dataSize);
	if (!ConvertHexToBytes(hex, data, dataSize)) return RET_ERR(-1, E_UNKNOWN);
	byte* dataEnd = &data[dataSize];

	// http://www.etsi.org/deliver/etsi_gts/03/0340/05.03.00_60/gsmts_0340v050300p.pdf
	// http://www.etsi.org/deliver/etsi_gts/03/0338/05.00.00_60/gsmts_0338v050000p.pdf
	byte* smscInfoSize = data;
	byte* tpMti = smscInfoSize + 1 + *smscInfoSize;
	if (tpMti >= dataEnd) return RET_ERR(-1, E_UNKNOWN);
	if ((*tpMti & 0xc0) != 0) return RET_ERR(-1, E_UNKNOWN);	// SMS-DELIVER
	byte* tpOaSize = tpMti + 1;
	if (tpOaSize >= dataEnd) return RET_ERR(-1, E_UNKNOWN);
	byte* tpPid = tpOaSize + 2 + *tpOaSize / 2 + *tpOaSize % 2;
	if (tpPid >= dataEnd) return RET_ERR(-1, E_UNKNOWN);
	byte* tpDcs = tpPid + 1;
	if (tpDcs >= dataEnd) return RET_ERR(-1, E_UNKNOWN);
	byte* tpScts = tpDcs + 1;
	if (tpScts >= dataEnd) return RET_ERR(-1, E_UNKNOWN);
	byte* tpUdl = tpScts + 7;
	if (tpUdl >= dataEnd) return RET_ERR(-1, E_UNKNOWN);
	byte* tpUd = tpUdl + 1;
	if (tpUd >= dataEnd) return RET_ERR(-1, E_UNKNOWN);

	if (dialNumber != NULL && dialNumberSize >= 1)
	{
		if (!SmAddressFieldToString(tpOaSize, dialNumber, dialNumberSize)) return RET_ERR(-1, E_UNKNOWN);
	}

	if (messageSize < *tpUdl + 1) return RET_ERR(-1, E_UNKNOWN);
	for (int i = 0; i < *tpUdl; i++) {
		int offset = i - i / 8;
		int shift = i % 8;
		if (shift == 0) {
			message[i] = tpUd[offset] & 0x7f;
		}
		else {
			message[i] = tpUd[offset] * 256 + tpUd[offset - 1] << shift >> 8 & 0x7f;
		}
	}
	message[*tpUdl] = '\0';

	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(*tpUdl);
}

bool WioLTE::DeleteReceivedSMS()
{
	int messageIndex = GetFirstIndexOfReceivedSMS();
	if (messageIndex == -2) return RET_ERR(false, E_UNKNOWN);
	if (messageIndex < 0) return RET_ERR(false, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+CMGD=%d", messageIndex)) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

bool WioLTE::Activate(const char* accessPointName, const char* userName, const char* password)
{
	const char* parameter;
	ArgumentParser parser;
	Stopwatch sw;

	sw.Start();
	while (true) {
		int resultCode;
		int status;

		_Module.WriteCommand("AT+CGREG?");
		if ((parameter = _Module.WaitForResponse(NULL, 500, "+CGREG: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false, E_UNKNOWN);
		parser.Parse(parameter);
		if (parser.Size() < 2) return RET_ERR(false, E_UNKNOWN);
		resultCode = atoi(parser[0]);
		status = atoi(parser[1]);
		if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
		if (status == 0) return RET_ERR(false, E_UNKNOWN);
		if (status == 1 || status == 5) break;

		_Module.WriteCommand("AT+CEREG?");
		if ((parameter = _Module.WaitForResponse(NULL, 500, "+CEREG: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false, E_UNKNOWN);
		parser.Parse(parameter);
		if (parser.Size() < 2) return RET_ERR(false, E_UNKNOWN);
		resultCode = atoi(parser[0]);
		status = atoi(parser[1]);
		if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
		if (status == 0) return RET_ERR(false, E_UNKNOWN);
		if (status == 1 || status == 5) break;

		if (sw.ElapsedMilliseconds() >= 120000) return RET_ERR(false, E_UNKNOWN);
	}

	// for debug.
#ifdef WIOLTE_DEBUG
	_Module.WriteCommandAndWaitForResponse("AT+CREG?", "OK", 500);
	_Module.WriteCommandAndWaitForResponse("AT+CGREG?", "OK", 500);
	_Module.WriteCommandAndWaitForResponse("AT+CEREG?", "OK", 500);
#endif // WIOLTE_DEBUG

	StringBuilder str;
	if (!str.WriteFormat("AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",1", accessPointName, userName, password)) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);

	sw.Start();
	while (true) {
		_Module.WriteCommand("AT+QIACT=1");
		const char* response = _Module.WaitForResponse("OK", 150000, "ERROR");
		if (response == NULL) return RET_ERR(false, E_UNKNOWN);
		if (strcmp(response, "OK") == 0) break;
		if (_Module.WriteCommandAndWaitForResponse("AT+QIGETERROR", "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
		if (sw.ElapsedMilliseconds() >= 150000) return RET_ERR(false, E_UNKNOWN);
		delay(POLLING_INTERVAL);
	}

	// for debug.
#ifdef WIOLTE_DEBUG
	if (_Module.WriteCommandAndWaitForResponse("AT+QIACT?", "OK", 150000) == NULL) return RET_ERR(false, E_UNKNOWN);
#endif // WIOLTE_DEBUG

	return RET_OK(true);
}

bool WioLTE::Deactivate()
{
	if (_Module.WriteCommandAndWaitForResponse("AT+QIDEACT=1", "OK", 40000) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

bool WioLTE::SyncTime(const char* host)
{
	const char* parameter;

	StringBuilder str;
	if (!str.WriteFormat("AT+QNTP=1,\"%s\"", host)) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);
	if ((parameter = _Module.WaitForResponse(NULL, 125000, "+QNTP: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

bool WioLTE::GetLocation(double* longitude, double* latitude)
{
	const char* parameter;
	ArgumentParser parser;

	if (_Module.WriteCommandAndWaitForResponse("AT+QLOCCFG=\"contextid\",1", "OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);

	_Module.WriteCommand("AT+QCELLLOC");
	if ((parameter = _Module.WaitForResponse(NULL, 60000, "+QCELLLOC: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false, E_UNKNOWN);
	parser.Parse(parameter);
	if (parser.Size() != 2) return RET_ERR(false, E_UNKNOWN);
	*longitude = atof(parser[0]);
	*latitude = atof(parser[1]);
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

int WioLTE::SocketOpen(const char* host, int port, SocketType type)
{
	if (host == NULL || host[0] == '\0') return RET_ERR(-1, E_UNKNOWN);
	if (port < 0 || 65535 < port) return RET_ERR(-1, E_UNKNOWN);

	const char* typeStr;
	switch (type) {
	case SOCKET_TCP:
		typeStr = "TCP";
		break;
	case SOCKET_UDP:
		typeStr = "UDP";
		break;
	default:
		return RET_ERR(-1, E_UNKNOWN);
	}

	bool connectIdUsed[CONNECT_ID_NUM];
	for (int i = 0; i < CONNECT_ID_NUM; i++) connectIdUsed[i] = false;

	_Module.WriteCommand("AT+QISTATE?");
	const char* response;
	ArgumentParser parser;
	do {
		if ((response = _Module.WaitForResponse("OK", 10000, "+QISTATE: ", ModuleSerial::WFR_START_WITH)) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (strncmp(response, "+QISTATE: ", 10) == 0) {
			parser.Parse(&response[10]);
			if (parser.Size() >= 1) {
				int connectId = atoi(parser[0]);
				if (connectId < 0 || CONNECT_ID_NUM <= connectId) return RET_ERR(-1, E_UNKNOWN);
				connectIdUsed[connectId] = true;
			}
		}
	} while (strcmp(response, "OK") != 0);

	int connectId;
	for (connectId = 0; connectId < CONNECT_ID_NUM; connectId++) {
		if (!connectIdUsed[connectId]) break;
	}
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QIOPEN=1,%d,\"%s\",\"%s\",%d", connectId, typeStr, host, port)) return RET_ERR(-1, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 150000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	str.Clear();
	if (!str.WriteFormat("+QIOPEN: %d,0", connectId)) return RET_ERR(-1, E_UNKNOWN);
	if (_Module.WaitForResponse(str.GetString(), 150000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(connectId);
}

bool WioLTE::SocketSend(int connectId, const byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(false, E_UNKNOWN);
	if (dataSize > 1460) return RET_ERR(false, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QISEND=%d,%d", connectId, dataSize)) return RET_ERR(false, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	if (_Module.WaitForResponse(NULL, 500, "> ", ModuleSerial::WFR_WITHOUT_DELIM) == NULL) return RET_ERR(false, E_UNKNOWN);
	_Module.Write(data, dataSize);
	if (_Module.WaitForResponse("SEND OK", 5000) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

bool WioLTE::SocketSend(int connectId, const char* data)
{
	return SocketSend(connectId, (const byte*)data, strlen(data));
}

int WioLTE::SocketReceive(int connectId, byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QIRD=%d", connectId)) return RET_ERR(-1, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	const char* parameter;
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+QIRD: ", ModuleSerial::WFR_START_WITH)) == NULL) return RET_ERR(-1, E_UNKNOWN);
	int dataLength = atoi(&parameter[7]);
	if (dataLength >= 1) {
		if (dataLength > dataSize) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.Read(data, dataLength, 500) != dataLength) return RET_ERR(-1, E_UNKNOWN);
	}
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(dataLength);
}

int WioLTE::SocketReceive(int connectId, char* data, int dataSize)
{
	int dataLength = SocketReceive(connectId, (byte*)data, dataSize - 1);
	if (dataLength >= 0) data[dataLength] = '\0';

	return dataLength;
}

int WioLTE::SocketReceive(int connectId, byte* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Start();
	int dataLength;
	while ((dataLength = SocketReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= timeout) return 0;
		delay(POLLING_INTERVAL);
	}
	return dataLength;
}

int WioLTE::SocketReceive(int connectId, char* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Start();
	int dataLength;
	while ((dataLength = SocketReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= timeout) return 0;
		delay(POLLING_INTERVAL);
	}
	return dataLength;
}

bool WioLTE::SocketClose(int connectId)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(false, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QICLOSE=%d", connectId)) return RET_ERR(false, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 10000) == NULL) return RET_ERR(false, E_UNKNOWN);

	return RET_OK(true);
}

int WioLTE::HttpGet(const char* url, char* data, int dataSize)
{
	const char* parameter;
	ArgumentParser parser;
	char* str;

	if (strncmp("https:", url, 6) == 0) {
		if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"sslctxid\",1"         , "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"sslversion\",1,4"      , "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"ciphersuite\",1,0XFFFF", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"seclevel\",1,0"        , "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
	}

	if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"requestheader\",0", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	if (!HttpSetUrl(url)) return RET_ERR(-1, E_UNKNOWN);

	if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPGET", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
	if ((parameter = _Module.WaitForResponse(NULL, 60000, "+QHTTPGET: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(-1, E_UNKNOWN);

	parser.Parse(parameter);
	if (parser.Size() < 1) return RET_ERR(-1, E_UNKNOWN);
	if (strcmp(parser[0], "0") != 0) return RET_ERR(-1, E_UNKNOWN);
	int contentLength = parser.Size() >= 3 ? atoi(parser[2]) : -1;

	_Module.WriteCommand("AT+QHTTPREAD");
	if (_Module.WaitForResponse("CONNECT", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	if (contentLength >= 0) {
		if (contentLength + 1 > dataSize) return RET_ERR(-1, E_UNKNOWN);
		int actualContentLength = 0;
		do {
			int readedContentLength = _Module.Read((byte*)&data[actualContentLength], contentLength - actualContentLength, 60000);
			if (readedContentLength <= 0) return RET_ERR(-1, E_UNKNOWN);
			actualContentLength += readedContentLength;
		} while (actualContentLength < contentLength);
		data[contentLength] = '\0';

		if (_Module.WaitForResponse("OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	}
	else {
		contentLength = 0;

		while (true) {
			parameter = _Module.WaitForResponse("OK", 60000, "", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_GET_NULL_STRING | ModuleSerial::WFR_TIMEOUT_FOR_BYTE));
			if (parameter == NULL) return RET_ERR(-1, E_UNKNOWN);
			if (strcmp(parameter, "OK") == 0) break;

			int parameterLength = strlen(parameter);
			if (contentLength + parameterLength + 2 + 1 > dataSize) return RET_ERR(-1, E_UNKNOWN);
			memcpy(&data[contentLength], parameter, parameterLength);
			strcpy(&data[contentLength + parameterLength], "\r\n");
			contentLength += parameterLength + 2;
		}
		if (contentLength >= 2 && strcmp(&data[contentLength - 2], "\r\n") == 0) contentLength -= 2;
		data[contentLength] = '\0';
	}
	if ((parameter = _Module.WaitForResponse(NULL, 1000, "+QHTTPREAD: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(-1, E_UNKNOWN);
	if (strcmp(parameter, "0") != 0) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(contentLength);
}

bool WioLTE::HttpPost(const char* url, const char* data, int* responseCode)
{
	const char* parameter;
	ArgumentParser parser;

	if (strncmp("https:", url, 6) == 0) {
		if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"sslctxid\",1"         , "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"sslversion\",1,4"      , "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"ciphersuite\",1,0XFFFF", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"seclevel\",1,0"        , "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);
	}

	if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"requestheader\",1", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	if (!HttpSetUrl(url)) return RET_ERR(false, E_UNKNOWN);

	const char* host;
	int hostLength;
	const char* uri;
	int uriLength;
	if (!SplitUrl(url, &host, &hostLength, &uri, &uriLength)) return RET_ERR(false, E_UNKNOWN);


	StringBuilder header;
	header.Write("POST ");
	if (uriLength <= 0) {
		header.Write("/");
	}
	else {
		header.Write(uri, uriLength);
	}
	header.Write(" HTTP/1.1\r\n");
	header.Write("Host: ");
	header.Write(host, hostLength);
	header.Write("\r\n");
	header.Write("Accept: */*\r\n");
	header.Write("User-Agent: "HTTP_POST_USER_AGENT"\r\n");
	header.Write("Connection: Keep-Alive\r\n");
	header.Write("Content-Type: "HTTP_POST_CONTENT_TYPE"\r\n");
	if (!header.WriteFormat("Content-Length: %d\r\n", strlen(data))) return RET_ERR(false, E_UNKNOWN);
	header.Write("\r\n");

	StringBuilder str;
	if (!str.WriteFormat("AT+QHTTPPOST=%d", header.Length() + strlen(data))) return RET_ERR(false, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	if (_Module.WaitForResponse("CONNECT", 60000) == NULL) return RET_ERR(false, E_UNKNOWN);
	_Module.Write(header.GetString());
	_Module.Write(data);
	if (_Module.WaitForResponse("OK", 1000) == NULL) return RET_ERR(false, E_UNKNOWN);

	if ((parameter = _Module.WaitForResponse(NULL, 60000, "+QHTTPPOST: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false, E_UNKNOWN);
	parser.Parse(parameter);
	if (parser.Size() < 1) return RET_ERR(-1, E_UNKNOWN);
	if (strcmp(parser[0], "0") != 0) return RET_ERR(-1, E_UNKNOWN);
	if (parser.Size() < 2) {
		*responseCode = -1;
	}
	else {
		*responseCode = atoi(parser[1]);
	}

	return RET_OK(true);
}

////////////////////////////////////////////////////////////////////////////////////////
//     IoTHub
////////////////////////////////////////////////////////////////////////////////////////

bool WioLTE::SetSSLCFG(const char* url)
{
	if (strncmp("https:", url, 6) == 0) {
		if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"sslctxid\",1"         , "OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"sslversion\",1,4"      , "OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"ciphersuite\",1,0XFFFF", "OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.WriteCommandAndWaitForResponse("AT+QSSLCFG=\"seclevel\",1,0"        , "OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	}

	return RET_OK(true);
}

bool WioLTE::IoTHubSend(const char* url, const char* data, const char* sas, int* responseCode)
{
	const char* parameter;
	ArgumentParser parser;

	if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"requestheader\",1", "OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	if (!HttpSetUrl(url)) return RET_ERR(-1, E_UNKNOWN);

	const char* host;
	int hostLength;
	const char* uri;
	int uriLength;
	if (!SplitUrl(url, &host, &hostLength, &uri, &uriLength)) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder header;
	header.Write("POST ");
	if (uriLength <= 0) {
		header.Write("/");
	}
	else {
		header.Write(uri, uriLength);
	}
	header.Write(" HTTP/1.1\r\n");
	header.Write("Host: ");
	header.Write(host, hostLength);
	header.Write("\r\n");
	header.Write("Accept: */*\r\n");
	header.Write("User-Agent: "HTTP_POST_USER_AGENT"\r\n");
	header.Write("Connection: Keep-Alive\r\n");
	header.WriteFormat("Authorization: %s\r\n",sas);
	header.Write("Content-Type: "HTTP_POST_CONTENT_TYPE"\r\n");
	if (!header.WriteFormat("Content-Length: %d\r\n", strlen(data))) return RET_ERR(-1, E_UNKNOWN); 
	header.Write("\r\n");

	StringBuilder str;
	if (!str.WriteFormat("AT+QHTTPPOST=%d", header.Length() + strlen(data))) return RET_ERR(-1, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	if (_Module.WaitForResponse("CONNECT", 60000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	_Module.Write(header.GetString());
	_Module.Write(data);
	if (_Module.WaitForResponse("OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	if ((parameter = _Module.WaitForResponse(NULL, 60000, "+QHTTPPOST: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(-1, E_UNKNOWN);
	parser.Parse(parameter);
	if (parser.Size() < 1) return RET_ERR(-1, E_UNKNOWN);
	if (strcmp(parser[0], "0") != 0) return RET_ERR(-1, E_UNKNOWN);
	if (parser.Size() < 2) {
		*responseCode = -1;
	}
	else {
		*responseCode = atoi(parser[1]);
	}

	return RET_OK(true);
}

int WioLTE::IoTHubRecieve(const char* url, char* data, const char* sas, int dataSize)
{
	const char* parameter;
	ArgumentParser parser;
	char* str;

	if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"requestheader\",1", "OK", 10000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse("AT+QHTTPCFG=\"responseheader\",1", "OK", 10000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	if (!HttpSetUrl(url)) return RET_ERR(-1, E_UNKNOWN);

	const char* host;
	int hostLength;
	const char* uri;
	int uriLength;
	
	if (!SplitUrl(url, &host, &hostLength, &uri, &uriLength)) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder header;
	header.Write("GET ");

	if (uriLength <= 0) {
		header.Write("/");
	}
	else {
		header.Write(uri, uriLength);
	}

	header.Write(" HTTP/1.1\r\n");
	header.Write("Host: ");
	header.Write(host, hostLength);
	header.Write("\r\n");
	header.WriteFormat("Authorization: %s\r\n",sas);
	header.Write("\r\n\r\n");

	StringBuilder strGet;
	if (!strGet.WriteFormat("AT+QHTTPGET=80,%d", header.Length())) return RET_ERR(-1, E_UNKNOWN);
	_Module.WriteCommand(strGet.GetString());
	if (_Module.WaitForResponse("CONNECT", 60000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	_Module.Write(header.GetString());
	if (_Module.WaitForResponse("OK", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	
	if ((parameter = _Module.WaitForResponse(NULL, 60000, "+QHTTPGET: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(-1, E_UNKNOWN);
	
	parser.Parse(parameter);
	if (parser.Size() < 1) return RET_ERR(-1, E_UNKNOWN);
	if (strcmp(parser[0], "0") != 0) return RET_ERR(-1, E_UNKNOWN);
	
	int contentLength = parser.Size() >= 3 ? atoi(parser[2]) : -1;
	
	_Module.WriteCommand("AT+QHTTPREAD");
	
	if (_Module.WaitForResponse("CONNECT", 1000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	char res[512];

	if (contentLength >= 0) {
		
		if (contentLength + 1 > dataSize) return RET_ERR(-1, E_UNKNOWN);
		
		_Module.Read((byte*)data, dataSize, 1000);
		res[dataSize] = '\0';

		if (strcmp(parser[1],"200")==0){

			char tmp[512];
			char* response[20];
			char* etag;
		
			strcpy(tmp,data);

			//SerialUSB.println(data);

			response[0]=strtok(tmp,"\r\n");

			for(int i=1;i<20;i++){
				response[i]=strtok(NULL,"\r\n");
			}
		
			etag=strtok(response[2],"\"");
			etag=strtok(NULL,"\"");

			String count=response[12];
			int l=count.length();
			count=count.substring(count.indexOf(":")+1,l);
			int cnt=count.toInt();
			
			String HostStr=url;
			int len=HostStr.length();
			HostStr=HostStr.substring(HostStr.indexOf(":")+3,len);
			String HostAddress=HostStr.substring(0,HostStr.indexOf("/"));
			HostStr=HostStr.substring(HostStr.indexOf("/")+1,len);
			String SendAddress="/"+HostStr.substring(0,HostStr.indexOf("?"))+"/";
			String ApiVer=HostStr.substring(HostStr.indexOf("?"),len);

			String delhost=SendAddress+etag+ApiVer;
			
			String delreq="DELETE "+delhost+" HTTP/1.1\r\n";
			
			delreq=delreq+"Host: "+HostAddress+"\r\n";
			delreq=delreq+"Authorization:"+sas+"\r\n\r\n\r\n";

			char buf[512];
			delreq.toCharArray(buf,512);
			
			char address[512];
			HostAddress.toCharArray(address,512);

			int connectId=SocketSslOpen(address,443);
			if (connectId<0){
				SerialUSB.println("### Socket Open Error ###");
			}

			if (!SocketSslSend(connectId,buf)){
				SerialUSB.println("### Socket Send Error ###");
			}

			if (!SocketSslClose(connectId)){
				SerialUSB.println("### Socket Close Error ###");
			}

			/*
			SerialUSB.println("------------------------");
			SerialUSB.print(etag);
			SerialUSB.print(" : ");
			SerialUSB.print(cnt);
			SerialUSB.print(" : ");
			SerialUSB.println(response[14]);
			SerialUSB.println("------------------------");
			*/
			
			strcpy(data,response[14]);
		}
	}

	return RET_OK(contentLength);

}

int WioLTE::SocketSslOpen(const char* host, int port)
{
	if (host == NULL || host[0] == '\0') return RET_ERR(-1, E_UNKNOWN);
	if (port < 0 || 65535 < port) return RET_ERR(-1, E_UNKNOWN);

	

	bool connectIdUsed[CONNECT_ID_NUM];
	for (int i = 0; i < CONNECT_ID_NUM; i++) connectIdUsed[i] = false;

	_Module.WriteCommand("AT+QSSLSTATE");
	const char* response;
	ArgumentParser parser;
	do {
		if ((response = _Module.WaitForResponse("OK", 10000, "+QSSLSTATE: ", ModuleSerial::WFR_START_WITH)) == NULL) return RET_ERR(-1, E_UNKNOWN);
		if (strncmp(response, "+QSSLSTATE: ", 10) == 0) {
			parser.Parse(&response[10]);
			if (parser.Size() >= 1) {
				int connectId = atoi(parser[0]);
				if (connectId < 0 || CONNECT_ID_NUM <= connectId) return RET_ERR(-1, E_UNKNOWN);
				connectIdUsed[connectId] = true;
			}
		}

	} while (strcmp(response, "OK") != 0);

	int connectId;
	for (connectId = 0; connectId < CONNECT_ID_NUM; connectId++) {
		if (!connectIdUsed[connectId]) break;
	}
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QSSLOPEN=1,1,%d,\"%s\",%d", connectId, host, port)) return RET_ERR(-1, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 150000) == NULL) return RET_ERR(-1, E_UNKNOWN);
	str.Clear();
	if (!str.WriteFormat("+QSSLOPEN: %d,0", connectId)) return RET_ERR(-1, E_UNKNOWN);
	if (_Module.WaitForResponse(str.GetString(), 150000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(connectId);
}

bool WioLTE::SocketSslSend(int connectId, const byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_UNKNOWN);
	if (dataSize > 1460) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QSSLSEND=%d,%d", connectId, dataSize)) return RET_ERR(-1, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	if (_Module.WaitForResponse(NULL, 500, "> ", ModuleSerial::WFR_WITHOUT_DELIM) == NULL) return RET_ERR(-1, E_UNKNOWN);
	_Module.Write(data, dataSize);
	if (_Module.WaitForResponse("SEND OK", 5000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(true);
}

bool WioLTE::SocketSslSend(int connectId, const char* data)
{
	return SocketSslSend(connectId, (const byte*)data, strlen(data));
}

int WioLTE::SocketSslReceive(int connectId, byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QIRD=%d,1460", connectId)) return RET_ERR(-1, E_UNKNOWN);
	_Module.WriteCommand(str.GetString());
	const char* parameter;
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+QIRD: ", ModuleSerial::WFR_START_WITH)) == NULL) return RET_ERR(-1, E_UNKNOWN);
	int dataLength = atoi(&parameter[7]);
	if (dataLength >= 1) {
		if (dataLength > dataSize) return RET_ERR(-1, E_UNKNOWN);
		if (_Module.Read(data, dataLength, 500) != dataLength) return RET_ERR(-1, E_UNKNOWN);
	}
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(dataLength);
}

int WioLTE::SocketSslReceive(int connectId, char* data, int dataSize)
{
	int dataLength = SocketSslReceive(connectId, (byte*)data, dataSize - 1);
	if (dataLength >= 0) data[dataLength] = '\0';

	return dataLength;
}

int WioLTE::SocketSslReceive(int connectId, byte* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Start();
	int dataLength;
	while ((dataLength = SocketSslReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= timeout) return 0;
		delay(POLLING_INTERVAL);
	}
	return dataLength;
}

int WioLTE::SocketSslReceive(int connectId, char* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Start();
	int dataLength;
	while ((dataLength = SocketSslReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= timeout) return 0;
		delay(POLLING_INTERVAL);
	}
	return dataLength;
}


bool WioLTE::SocketSslClose(int connectId)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+QSSLCLOSE=%d", connectId)) return RET_ERR(-1, E_UNKNOWN);
	if (_Module.WriteCommandAndWaitForResponse(str.GetString(), "OK", 10000) == NULL) return RET_ERR(-1, E_UNKNOWN);

	return RET_OK(true);
}


////////////////////////////////////////////////////////////////////////////////////////

