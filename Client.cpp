#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <Ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

const std::uint32_t datagrammSize = 65500;
const std::uint64_t fileSizeLimit = 10485760; // 10 megabytes 
const int datagrammIDSize = 2; // 4 bytes limit for addDatagrammID()

std::uint32_t createDatagrammID()
{
	static std::uint32_t generateID = 0;
	return generateID++;
}

void addDatagrammID(char* buf, int datagrammDataSize)
{
	std::uint32_t datagrammID = createDatagrammID(); // unsigned int means 4 byte limit for datagrammIDSize
	for (int i = 0; i < datagrammIDSize; ++i)
	{
		buf[datagrammDataSize+i] = datagrammID; // add ID to the end of datagramm
		datagrammID >>= 8;
	}
}

void splitFile(std::ifstream& file, std::vector<char*>& container, std::uint32_t datagrammDataSize, std::uint32_t iterations)
{
	char* buf = nullptr;
	for (std::uint32_t i = 0; i < iterations; ++i)
	{
		buf = new char[datagrammDataSize + datagrammIDSize];
		file.read(buf, datagrammDataSize);
		addDatagrammID(buf, datagrammDataSize);
		container.push_back(buf);
	}
	buf = nullptr;
}

int main(int argc, char* argv[])
{
	const char* IP = argv[1];
	const char* connectionPort = argv[2];
	const char* UDPPort = argv[3];
	const char* transmissionFileName = argv[4];
	const std::string UDPtimeout = argv[5];

	
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult)
	{
		std::cout << "WSAStartup() failed with error " << iResult;
		exit(1);
	}

	// TCP connection
	ADDRINFO hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	ADDRINFO* addrResult = nullptr;
	iResult = getaddrinfo(IP, connectionPort, &hints, &addrResult);
	if (iResult)
	{
		std::cout << "getaddrinfo() failed with error " << iResult;
		WSACleanup();
		exit(1);
	}

	SOCKET socketTCP = INVALID_SOCKET;
	socketTCP = socket(addrResult->ai_family, addrResult->ai_socktype, addrResult->ai_protocol);
	if (socketTCP == INVALID_SOCKET)
	{
		std::cout << "Socket creation failed";
		freeaddrinfo(addrResult);
		WSACleanup();
		exit(1);
	}

	iResult = connect(socketTCP, addrResult->ai_addr, (int)addrResult->ai_addrlen);
	if (iResult == SOCKET_ERROR)
	{
		std::cout << "Connection to server failed\n";
		closesocket(socketTCP);
		freeaddrinfo(addrResult);
		WSACleanup();
		exit(1);
	}
	else { std::cout << "Connected\n";}
	
	// Open file and check filesize
	std::ifstream transmissionFile (transmissionFileName, std::ios::binary);
	transmissionFile.seekg(0, std::ios::end);
	std::uint64_t fileSize = transmissionFile.tellg();
	
	if (fileSize > fileSizeLimit)
	{
		std::cout << "Uploaded file is too big";
		transmissionFile.close();
		closesocket(socketTCP);
		freeaddrinfo(addrResult);
		WSACleanup();
		exit(1);
	}
	else {transmissionFile.seekg(0, std::ios::beg);}
	
	// Sending filename and UDP port
	int fileNameSize = strlen(transmissionFileName) + 1;
	send(socketTCP, (char*)&fileNameSize, sizeof(fileNameSize), NULL);
	send(socketTCP, transmissionFileName, fileNameSize, NULL);
	
	int UDPPortSize = strlen(UDPPort) + 1;
	send(socketTCP, (char*)&UDPPortSize, sizeof(UDPPortSize), NULL);
	send(socketTCP, UDPPort, UDPPortSize, NULL);

	// Creating UDP socket
	ADDRINFO hintsUDP;
	ZeroMemory(&hintsUDP, sizeof(hintsUDP));
	hintsUDP.ai_family = AF_INET;
	hintsUDP.ai_socktype = SOCK_DGRAM;
	hintsUDP.ai_protocol = IPPROTO_UDP;
	ADDRINFO* addrResultUDP = nullptr;
	iResult = getaddrinfo(IP, UDPPort, &hintsUDP, &addrResultUDP);
	if (iResult)
	{
		std::cout << "getaddrinfo() failed with error " << iResult;
		closesocket(socketTCP);
		freeaddrinfo(addrResult);
		WSACleanup();
		exit(1);
	}

	SOCKET socketUDP = INVALID_SOCKET;
	socketUDP = socket(addrResultUDP->ai_family, addrResultUDP->ai_socktype, (int)addrResultUDP->ai_protocol);
	if (socketUDP == INVALID_SOCKET)
	{
		std::cout << "Socket creation failed";
		closesocket(socketTCP);
		freeaddrinfo(addrResultUDP);
		freeaddrinfo(addrResult);
		WSACleanup();
		exit(1);
	}
	
	// Sending fileSize
	char fileSizeBuf[sizeof(std::uint64_t)];
	std::uint64_t tempfileSize = fileSize;
	for (int i = 0; i < sizeof(std::uint64_t); ++i)
	{
		fileSizeBuf[i] = tempfileSize;
		tempfileSize >>= 8;
	}
	send(socketTCP, fileSizeBuf, sizeof(std::uint64_t), NULL);
	
	// Splitting file
	std::uint32_t countOfEqualFragments = fileSize / datagrammSize;
	std::uint32_t sizeOfModulo = fileSize % datagrammSize;
	std::vector<char*> datagramms;
	
	if (fileSize >= datagrammSize)
	{
		splitFile(transmissionFile, datagramms, datagrammSize, countOfEqualFragments);
		if (sizeOfModulo) {splitFile(transmissionFile, datagramms, sizeOfModulo, 1);}	
	}
	else 
	{
		splitFile(transmissionFile, datagramms, fileSize, 1);
	}
	
	// Closing file
	transmissionFile.close();

	// Setting timeout on TCP socket
	DWORD timeout = std::stoi(UDPtimeout);
	setsockopt(socketTCP, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
	
	// Sending datagramms
	bool ACK = false;
		
	if (fileSize >= datagrammSize)
	{
		for (std::uint32_t i = 0; i < countOfEqualFragments; ++i)
		{
			ACK = false;
			do
			{
				sendto(socketUDP, datagramms[i], datagrammSize + datagrammIDSize, NULL, addrResultUDP->ai_addr, addrResultUDP->ai_addrlen);
				recv(socketTCP, (char*)&ACK, sizeof(ACK), NULL);

			} while (ACK == false);
			
		}
		
		if (sizeOfModulo)
		{
			ACK = false;
			do
			{
				sendto(socketUDP, datagramms.back(), sizeOfModulo + datagrammIDSize, NULL, addrResultUDP->ai_addr, addrResultUDP->ai_addrlen);
				recv(socketTCP, (char*)&ACK, sizeof(ACK), NULL);

			} while (ACK == false);
		}
	}
	else
	{
		ACK = false;
		do
		{
			sendto(socketUDP, datagramms.back(), fileSize + datagrammIDSize, NULL, addrResultUDP->ai_addr, addrResultUDP->ai_addrlen);
			recv(socketTCP, (char*)&ACK, sizeof(ACK), NULL);

		} while (ACK == false);
	}
	
	bool transmissionFileSuccess = true;
	send(socketTCP, (char*)&transmissionFileSuccess, sizeof(transmissionFileSuccess), NULL);
	
	// Freeing memory 
	for (auto in : datagramms)
	{
		delete[] in;
		in = nullptr;
	}
	closesocket(socketUDP);
	closesocket(socketTCP);
	freeaddrinfo(addrResultUDP);
	freeaddrinfo(addrResult);
	WSACleanup();
	
	
	return 0;
}