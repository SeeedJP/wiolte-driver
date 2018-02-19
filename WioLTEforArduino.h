#pragma once

#include "config.h"
#include <Arduino.h>
#include <Seeed_ws2812.h>
#include <time.h>
#include <vector>

#define WIOLTE_TCP	(WioLTE::SOCKET_TCP)
#define WIOLTE_UDP	(WioLTE::SOCKET_UDP)

#define WIOLTE_D38	(WioLTE::D38)
#define WIOLTE_D39	(WioLTE::D39)
#define WIOLTE_D20	(WioLTE::D20)
#define WIOLTE_D19	(WioLTE::D19)
#define WIOLTE_A6	(WioLTE::A6)
#define WIOLTE_A7	(WioLTE::A7)
#define WIOLTE_A4	(WioLTE::A4)
#define WIOLTE_A5	(WioLTE::A5)

class WioLTE
{
	/////////////////////////////////////////////////////////////////////
	// Stopwatch

private:
	class Stopwatch
	{
	private:
		unsigned long _BeginTime;
		unsigned long _EndTime;

	public:
		Stopwatch() : _BeginTime(0), _EndTime(0)
		{
		}

		void Start()
		{
			_BeginTime = millis();
			_EndTime = 0;
		}

		void Stop()
		{
			_EndTime = millis();
		}

		unsigned long ElapsedMilliseconds() const
		{
			if (_EndTime == 0) {
				return millis() - _BeginTime;
			}
			else {
				return _EndTime - _BeginTime;
			}
		}

	};

	/////////////////////////////////////////////////////////////////////
	// ArgumentParser

private:
	class ArgumentParser
	{
	private:
		std::vector< std::vector<char> > _Arguments;

	public:
		ArgumentParser();
		void Parse(const char* str);
		int Size() const;
		const char* operator[](int index) const;

	};

	/////////////////////////////////////////////////////////////////////
	// StringBuilder

private:
	class StringBuilder
	{
	private:
		std::vector<char> _Buffer;

	public:
		StringBuilder();
		void Clear();
		int Length() const;
		const char* GetString() const;
		void Write(const char* str);
		void Write(const char* str, int length);
		bool WriteFormat(const char* format, ...);

	};

	/////////////////////////////////////////////////////////////////////
	// ModuleSerial

private:
	class ModuleSerial
	{
	private:
		void SerialInit() { Serial1.begin(115200); }
		bool SerialAvailable() const { return Serial1.available() >= 1 ? true : false; }
		byte SerialRead() { return Serial1.read(); }
		void SerialWrite(byte data) { Serial1.write(data); }
		void SerialWrite(const byte* data, int dataSize) { Serial1.write(data, dataSize); }

	public:
		enum WaitForResponseFlag {
			WFR_WITHOUT_DELIM		= 0x01,
			WFR_START_WITH			= 0x02,
			WFR_REMOVE_START_WITH	= 0x04,
			WFR_GET_NULL_STRING		= 0x08,
			WFR_TIMEOUT_FOR_BYTE	= 0x10,
		};

	public:
		ModuleSerial();

	private:
		std::vector<char> _LastResponse;

		void DiscardRead();
		bool WaitForAvailable(Stopwatch* sw, long timeout) const;
		int Read(byte* data, int dataSize);
		const char* ReadResponse(const char* match, long timeout);

	public:
		void Init();
		void Write(const byte* data, int dataSize);
		void Write(const char* str);
		int Read(byte* data, int dataSize, long timeout);

		void WriteCommand(const char* command);
		const char* WaitForResponse(const char* waitResponse, long timeout, const char* waitPattern = NULL, WaitForResponseFlag waitPatternFlag = (WaitForResponseFlag)0);
		const char* WriteCommandAndWaitForResponse(const char* command, const char* response, long timeout);

	};

	/////////////////////////////////////////////////////////////////////
	// WioLTE

public:
	enum ErrorCodeType {
		E_OK = 0,
		E_UNKNOWN,
		E_
	};

	enum SocketType {
		SOCKET_TCP,
		SOCKET_UDP,
	};

private:
#if defined WIOLTE_SCHEMATIC_A
	static const int MODULE_PWR_PIN = 18;		// PB2
	static const int ANT_PWR_PIN = 28;			// PB12
	static const int ENABLE_VCCB_PIN = 26;		// PB10    

	static const int PWR_KEY_PIN = 36;			// PC4 
	static const int RESET_MODULE_PIN = 35;		// PC3

	static const int STATUS_PIN = 31;			// PB15

	static const int DTR_PIN = 1;				// PA1

	static const int WAKEUP_IN_PIN = 32;		// PC0
	static const int W_DISABLE_PIN = 34;		// PC2
	static const int AP_READY_PIN = 33;			// PC1

	static const int RGB_LED_PIN = 17;			// PB1

#elif defined WIOLTE_SCHEMATIC_B
	static const int MODULE_PWR_PIN = 21;		// PB5
	static const int ANT_PWR_PIN = 28;			// PB12
	static const int ENABLE_VCCB_PIN = 26;		// PB10
	static const int RGB_LED_PWR_PIN = 8;		// PA8

	static const int PWR_KEY_PIN = 36;			// PC4 
	static const int RESET_MODULE_PIN = 35;		// PC3

	static const int STATUS_PIN = 31;			// PB15

	static const int DTR_PIN = 1;				// PA1

	static const int WAKEUP_IN_PIN = 32;		// PC0
	static const int W_DISABLE_PIN = 34;		// PC2
	static const int AP_READY_PIN = 33;			// PC1

	static const int RGB_LED_PIN = 17;			// PB1
	static const int SD_POWR_PIN = 15;			// PA15

#else
#error Unkown WioLTE type

#endif

public:
	// D38 connector
	static const int D38 = 38;
	static const int D39 = 39;

	// D20 connector
	static const int D20 = 20;
	static const int D19 = 19;

	// A6 connector
	static const int A6 = 6;
	static const int A7 = 7;

	// A4 connector
	static const int A4 = 4;
	static const int A5 = 5;

private:
	ModuleSerial _Module;
	WS2812 _Led;
	ErrorCodeType _LastErrorCode;

private:
	bool ReturnOk(bool value)
	{
		_LastErrorCode = E_OK;
		return value;
	}
	int ReturnOk(int value)
	{
		_LastErrorCode = E_OK;
		return value;
	}
	bool ReturnError(int lineNumber, bool value, ErrorCodeType errorCode);
	int ReturnError(int lineNumber, int value, ErrorCodeType errorCode);

	bool IsBusy() const;
	bool Reset();
	bool TurnOn();

	int GetFirstIndexOfReceivedSMS();

	bool HttpSetUrl(const char* url);

public:
	WioLTE();
	ErrorCodeType GetLastError() const;
	void Init();
	void LedSetRGB(byte red, byte green, byte blue);
	void PowerSupplyLTE(bool on);
	void PowerSupplyGNSS(bool on);
	void PowerSupplyGrove(bool on);
	void PowerSupplySD(bool on);
	bool TurnOnOrReset();
	bool TurnOff();
	bool Sleep();
	bool Wakeup();

	int GetIMEI(char* imei, int imeiSize);
	int GetIMSI(char* imsi, int imsiSize);
	int GetPhoneNumber(char* number, int numberSize);
	int GetReceivedSignalStrength();
	bool GetTime(struct tm* tim);

	//bool Call(const char* dialNumber);
	//void IsRinging();
	//void Answer();
	//bool HangUp();

	bool SendSMS(const char* dialNumber, const char* message);
	int ReceiveSMS(char* message, int messageSize, char* dialNumber = NULL, int dialNumberSize = 0);
	bool DeleteReceivedSMS();

	bool Activate(const char* accessPointName, const char* userName, const char* password);
	bool Deactivate();

	bool SyncTime(const char* host);
	bool GetLocation(double* longitude, double* latitude);

	int SocketOpen(const char* host, int port, SocketType type);
	bool SocketSend(int connectId, const byte* data, int dataSize);
	bool SocketSend(int connectId, const char* data);
	int SocketReceive(int connectId, byte* data, int dataSize);
	int SocketReceive(int connectId, char* data, int dataSize);
	int SocketReceive(int connectId, byte* data, int dataSize, long timeout);
	int SocketReceive(int connectId, char* data, int dataSize, long timeout);
	bool SocketClose(int connectId);

	int HttpGet(const char* url, char* data, int dataSize);
	bool HttpPost(const char* url, const char* data, int* responseCode);

	//IoTHub
	bool SetSSLCFG(const char* url);
	bool IoTHubSend(const char* url, const char* data, const char* sas , int* responseCode);
	int IoTHubRecieve(const char* url, char* data, const char* sas, int dataSize);
	int SocketSslOpen(const char* host, int port);
	bool SocketSslSend(int connectId, const byte* data, int dataSize);
	bool SocketSslSend(int connectId, const char* data);
	int SocketSslReceive(int connectId, byte* data, int dataSize);
	int SocketSslReceive(int connectId, char* data, int dataSize);
	int SocketSslReceive(int connectId, byte* data, int dataSize, long timeout);
	int SocketSslReceive(int connectId, char* data, int dataSize, long timeout);
	bool SocketSslClose(int connectId);

};
